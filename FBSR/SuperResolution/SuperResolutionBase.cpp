#include <highgui/highgui.hpp>
#include <contrib/contrib.hpp>
#include <iostream>

#include "SuperResolutionBase.h"
#include "ReadEmilyImageList.hpp"
#include "../LKOFlow/LKOFlow.h"
#include "../Utils/Utils.hpp"

SuperResolutionBase::SuperResolutionBase(int bufferSize) : isFirstRun(false), bufferSize(bufferSize), srFactor(4), psfSize(3), psfSigma(1.0)
{
	this->frameBuffer = new FrameBuffer(bufferSize);
}

bool SuperResolutionBase::SetFrameSource(const cv::Ptr<FrameSource>& frameSource)
{
	this->frameSource = frameSource;
	this->isFirstRun = true;
	return true;
}

void SuperResolutionBase::SetProps(double alpha, double beta, double lambda, double P, int maxIterationCount)
{
	props.alpha = alpha;
	props.beta = beta;
	props.lambda = lambda;
	props.P = P;
	props.maxIterationCount = maxIterationCount;
}

bool SuperResolutionBase::Reset()
{
	this->frameSource->reset();
	this->isFirstRun = true;
	return true;
}

void SuperResolutionBase::NextFrame(OutputArray outputFrame)
{
	isFirstRun = false;
	if (isFirstRun)
	{
		Init(this->frameSource);
		isFirstRun = false;
	}
	SetProps(0.7, 1, 0.04, 2, 20);
	Process(this->frameSource, outputFrame);
}

void SuperResolutionBase::Init(Ptr<FrameSource>& frameSource)
{
	Mat currentFrame;
	SetProps(0.7, 1, 0.04, 2, 20);

	for (auto i = 0; i < bufferSize; ++i)
	{
		frameSource->nextFrame(currentFrame);
		frameBuffer->PushGray(currentFrame);
	}

	frameSize = Size(currentFrame.rows, currentFrame.cols);
	currentFrame.release();
}

void SuperResolutionBase::UpdateZAndA(Mat& Z, Mat& A, int x, int y, const vector<bool>& index, const vector<Mat>& frames, const int len) const
{
	vector<Mat> selectedFrames;
	for (auto i = 0; i < index.size(); ++i)
	{
		if (true == index[i])
			selectedFrames.push_back(frames[i]);
	}

	Mat mergedFrame;
	merge(selectedFrames, mergedFrame);

	Mat medianFrame(frames[0].rows, frames[0].cols, CV_32FC1);
	Utils::CalculatedMedian(mergedFrame, medianFrame);

	for (auto r = x - 1; r < Z.rows-3; r += srFactor)
	{
		auto rowOfMatZ = Z.ptr<float>(r);
		auto rowOfMatA = A.ptr<float>(r);
		auto rowOfMedianFrame = medianFrame.ptr<float>(r / srFactor);

		for (auto c = y - 1; c < Z.cols-3; c += srFactor)
		{
			rowOfMatZ[c] = rowOfMedianFrame[c / srFactor];
			rowOfMatA[c] = static_cast<float>(len);
		}
	}
}

void SuperResolutionBase::MedianAndShift(const vector<Mat>& interp_previous_frames, const vector<vector<double>>& current_distances, const Size& new_size, Mat& Z, Mat& A) const
{
	Z = Mat::zeros(new_size, CV_32FC1);
	A = Mat::ones(new_size, CV_32FC1);

	Mat markMat = Mat::zeros(Size(srFactor, srFactor), CV_8UC1);

	for (auto x = srFactor; x < 2 * srFactor; ++x)
	{
		for (auto y = srFactor; y < 2 * srFactor; ++y)
		{
			vector<bool> index;
			for (auto k = 0; k < current_distances.size(); ++k)
			{
				if (current_distances[k][0] == x && current_distances[k][1] == y)
					index.push_back(true);
				else
					index.push_back(false);
			}

			auto len = Utils::CalculateCount(index, true);
			if (len > 0)
			{
				markMat.at<uchar>(x - srFactor, y - srFactor) = 1;
				UpdateZAndA(Z, A, x, y, index, interp_previous_frames, len);
			}
		}
	}

	Mat noneZeroMapOfMarkMat = markMat == 0;

	vector<int> X, Y;
	for (auto r = 0; r < noneZeroMapOfMarkMat.rows; ++r)
	{
		auto perRow = noneZeroMapOfMarkMat.ptr<uchar>(r);
		for (auto c = 0; c < noneZeroMapOfMarkMat.cols; ++c)
		{
			if (static_cast<int>(perRow[c]) != 0)
			{
				X.push_back(c);
				Y.push_back(r);
			}
		}
	}

	if (X.size() != 0)
	{
		Mat meidianBlurMatOfMatZ;
		medianBlur(Z, meidianBlurMatOfMatZ, 3);

		auto rowCount = Z.rows;
		auto colCount = Z.cols;

		for (auto i = 0; i < X.size(); ++i)
		{
			for (auto r = Y[i] + srFactor - 1; r < rowCount; r += srFactor)
			{
				auto rowOfMatZ = Z.ptr<float>(r);
				auto rowOfMedianBlurMatOfMatZ = meidianBlurMatOfMatZ.ptr<float>(r);

				for (auto c = X[i] + srFactor - 1; c < colCount; c += srFactor)
					rowOfMatZ[c] = rowOfMedianBlurMatOfMatZ[c];
			}
		}
	}

	Mat rootedMatA;
	sqrt(A, rootedMatA);
	rootedMatA.copyTo(A);
	rootedMatA.release();
}

Mat SuperResolutionBase::FastGradientBackProject(const Mat& Xn, const Mat& Z, const Mat& A, const Mat& hpsf)
{
	Mat matZAfterGaussianFilter;
	filter2D(Xn, matZAfterGaussianFilter, CV_32FC1, hpsf, Point(-1, -1), 0, BORDER_REFLECT);

	Mat diffOfZandMedianFiltedZ;
	subtract(matZAfterGaussianFilter, Z, diffOfZandMedianFiltedZ);

	Mat multiplyOfdiffZAndA = A.mul(diffOfZandMedianFiltedZ);

	Mat Gsign(multiplyOfdiffZAndA.rows, multiplyOfdiffZAndA.cols, CV_32FC1);
	Utils::Sign(multiplyOfdiffZAndA, Gsign);

	Mat inversedHpsf;
	flip(hpsf, inversedHpsf, -1);
	Mat multiplyOfGsingAndMatA = A.mul(Gsign);

	Mat filterResult;
	filter2D(multiplyOfGsingAndMatA, filterResult, CV_32FC1, inversedHpsf, Point(-1, -1), 0, BORDER_REFLECT);

	return filterResult;
}

Mat SuperResolutionBase::GradientRegulization(const Mat& Xn, const double P, const double alpha) const
{
	Mat G = Mat::zeros(Xn.rows, Xn.cols, CV_32FC1);

	Mat paddedXn;
	copyMakeBorder(Xn, paddedXn, P, P, P, P, BORDER_REFLECT);

	for (int i = -1 * P; i <= P; ++i)
	{
		for (int j = -1 * P; j <= P; ++j)
		{
			Rect shiftedXnRect(Point(0 + P - i, 0 + P - j), Point(paddedXn.cols - P - i, paddedXn.rows - P - j));
			auto shiftedXn = paddedXn(shiftedXnRect);

			Mat diffOfXnAndShiftedXn = Xn - shiftedXn;
			Mat signOfDiff(diffOfXnAndShiftedXn.rows, diffOfXnAndShiftedXn.cols, CV_32FC1);
			Utils::Sign(diffOfXnAndShiftedXn, signOfDiff);

			Mat paddedSignOfDiff;
			copyMakeBorder(signOfDiff, paddedSignOfDiff, P, P, P, P, BORDER_CONSTANT, 0);

			Rect shiftedSignedOfDiffRect(Point(0 + P + i, 0 + P + j), Point(paddedSignOfDiff.cols - P + i, paddedSignOfDiff.rows - P + j));
			auto shiftedSignOfDiff = paddedSignOfDiff(shiftedSignedOfDiffRect);

			Mat diffOfSignAndShiftedSign = signOfDiff - shiftedSignOfDiff;

			auto tempScale = pow(alpha, (abs(i) + abs(j)));
			diffOfSignAndShiftedSign *= tempScale;

			G += diffOfSignAndShiftedSign;
		}
	}
	return G;
}

Mat SuperResolutionBase::FastRobustSR(const vector<Mat>& interp_previous_frames, const vector<vector<double>>& current_distances, Mat hpsf)
{
	Mat Z, A;
	Size newSize((frameSize.width + 1) * srFactor - 1, (frameSize.height + 1) * srFactor - 1);
	MedianAndShift(interp_previous_frames, current_distances, newSize, Z, A);

	Mat HR;
	Z.copyTo(HR);

	auto iter = 1;

	while (iter < props.maxIterationCount)
	{
		auto Gback = FastGradientBackProject(HR, Z, A, hpsf);
		auto Greg = GradientRegulization(HR, props.P, props.alpha);

		Greg *= props.lambda;
		Mat tempResultOfIteration = (Gback + Greg) * props.beta;
		HR -= tempResultOfIteration;

		++iter;
	}
	return HR;
}

vector<Mat> SuperResolutionBase::NearestInterp2(const vector<Mat>& previousFrames, const vector<vector<double>>& distances) const
{
	Mat X, Y;
	LKOFlow::Meshgrid(0, frameSize.width - 1, 0, frameSize.height - 1, X, Y);

	vector<Mat> result;
	result.resize(previousFrames.size());

	for (auto i = 0; i < distances.size(); ++i)
	{
		Mat shiftX = X + distances[i][0];
		Mat shiftY = Y + distances[i][0];

		auto currentFrame = previousFrames[i];
		remap(currentFrame, result[i], shiftX, shiftY, INTER_NEAREST);
	}
	return result;
}

void SuperResolutionBase::Process(Ptr<FrameSource>& frameSource, OutputArray outputFrame)
{
	/************************************************************************************
	 *
	 * Set Prarameters for Test Case
	 *
	 ***********************************************************************************/
	bufferSize = 53;
	auto emilyImageCount = 53;
	vector<Mat> EmilyImageList;
	EmilyImageList.resize(emilyImageCount);
	ReadEmilyImageList::ReadImageList(EmilyImageList, emilyImageCount);

	/**********************************************************************************
	 *
	 * Read Image List and Register them
	 *
	 *********************************************************************************/
	frameSize = Size(EmilyImageList[0].cols, EmilyImageList[0].rows);
	auto registeredDistances = RegisterImages(EmilyImageList);

	vector<vector<double>> roundedDistances(registeredDistances.size(), vector<double>(2, 0.0));
	vector<vector<double>> restedDistances(registeredDistances.size(), vector<double>(2, 0.0));
	ReCalculateDistances(registeredDistances, roundedDistances, restedDistances);

	auto interpPreviousFrames = NearestInterp2(EmilyImageList, restedDistances);

	auto Hpsf = Utils::GetGaussianKernal(psfSize, psfSigma);

	auto Hr = FastRobustSR(interpPreviousFrames, roundedDistances, Hpsf);

	Mat UcharHr;
	Hr.convertTo(UcharHr, CV_8UC1);
	imshow("Test", UcharHr);
	waitKey(0);

	destroyAllWindows();

//	Mat currentFrame;
//	while (frameBuffer->CurrentFrame().data)
//	{
//		auto previous_frames = frameBuffer->GetAll();
//		auto currentDistances = RegisterImages(previous_frames);
//		auto restDistances = ReCalculateDistances(currentDistances);
//		auto interpPreviousFrames = NearestInterp2(previous_frames, restDistances);
//		auto Hpsf = GetGaussianKernal();
//		auto Hr = FastRobustSR(interpPreviousFrames, currentDistances, Hpsf);
//		cout << Hr(Rect(0, 0, 16, 16)) << endl;
//		cout << endl;
//		Mat UcharHr;
//		Hr.convertTo(UcharHr, CV_8UC1);
//		 for (auto i = 0; i < bufferSize; ++i)
//		{
//			imshow("Previous Frames", PreviousFrames[i]);
//			waitKey(100);
//		}
//		cout << UcharHr(Rect(0, 0, 16, 16)) << endl;
//		frameSource->nextFrame(currentFrame);
//		frameBuffer->PushGray(currentFrame);
//	}
//	currentFrame.release();
//	destroyAllWindows();
}

vector<vector<double>> SuperResolutionBase::RegisterImages(vector<Mat>& frames)
{
	vector<vector<double>> result;
	Rect rectROI(0, 0, frames[0].cols, frames[0].rows);

	result.push_back(vector<double>(2, 0.0));

	for (auto i = 1; i < frames.size(); ++i)
	{
		auto currentDistance = LKOFlow::PyramidalLKOpticalFlow(frames[0], frames[i], rectROI);
		result.push_back(currentDistance);
	}

	return result;
}

void SuperResolutionBase::GetRestDistance(const vector<vector<double>>& roundedDistances, vector<vector<double>>& restedDistances, int srFactor) const
{
	for (auto i = 0; i < roundedDistances.size(); ++i)
		for (auto j = 0; j < roundedDistances[0].size(); ++j)
			restedDistances[i][j] = floor(roundedDistances[i][j] / srFactor);
}

void SuperResolutionBase::RoundAndScale(const vector<vector<double>>& registeredDistances, vector<vector<double>>& roundedDistances, int srFactor) const
{
	for (auto i = 0; i < registeredDistances.size(); ++i)
		for (auto j = 0; j < registeredDistances[0].size(); ++j)
			roundedDistances[i][j] = round(registeredDistances[i][j] * double(srFactor));
}

void SuperResolutionBase::ModAndAddFactor(vector<vector<double>>& roundedDistances, int srFactor) const
{
	for (auto i = 0; i < roundedDistances.size(); ++i)
		for (auto j = 0; j < roundedDistances[0].size(); ++j)
			roundedDistances[i][j] = fmod(roundedDistances[i][j], static_cast<double>(srFactor)) + srFactor;
}

void SuperResolutionBase::ReCalculateDistances(const vector<vector<double>>& registeredDistances, vector<vector<double>>& roundedDistances, vector<vector<double>>& restedDistances) const
{
	// NOTE: Cannot change order

	RoundAndScale(registeredDistances, roundedDistances, srFactor);

	GetRestDistance(roundedDistances, restedDistances, srFactor);

	ModAndAddFactor(roundedDistances, srFactor);
}
