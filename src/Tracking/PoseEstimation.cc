/**
* This file is part of CNN-SLAM.
*
* [Copyright of CNN-SLAM]
* Copyright (C) 2018
* Kai Yu <kaiy1 at andrew dot cmu dot edu> (Carnegie Mellon University)
* Zhongxu Wang <zhongxuw at andrew dot cmu dot edu> (Carnegie Mellon University)
* Manchen Wang <manchen2 at andrew dot cmu dot edu> (Carnegie Mellon University)
* For more information see <https://github.com/raulmur/CNN_SLAM>
*
* CNN-SLAM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CNN-SLAM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CNN-SLAM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <Tracking/PoseEstimation.h>
#include <KeyFrame.h>

#include <ceres/autodiff_cost_function.h>
#include <ceres/numeric_diff_cost_function.h>
#include <ceres/sized_cost_function.h>
#include <ceres/solver.h>
#include <ceres/loss_function.h>
#include <ceres/problem.h>
#include <ceres/rotation.h>

#include <thread>
#include <util/settings.h>
#include <Frame.h>

using namespace cv;
using namespace std;

namespace cnn_slam {

    // Use ceres for Gaussian-Newton optimization.
    using namespace ceres;

    inline float RotationAngle(const cv::Mat &R) {
        return static_cast<float>(std::acos((trace(R)[0] - 1) / 2));
    }

    inline float TranslationDist(const cv::Mat &t) {
        return static_cast<float>(cv::norm(t));
    }

    struct CostFunctor {
        Mat imColor;
        ORB_SLAM2::KeyFrame *pReferenceKF;
        Mat Kt;      // Transposed calibration matrix.
        Mat invKt;   // Transposed inverse calibration matrix.
        float cameraPixelNoise2;

        CostFunctor(Mat imColor,
                    ORB_SLAM2::KeyFrame *pReferenceKF,
                    Mat K,
                    Mat invK,
                    float cameraPixelNoise2)
                : imColor(imColor),
                  pReferenceKF(pReferenceKF),
                  Kt(K.t()),
                  invKt(invK.t()),
                  cameraPixelNoise2(cameraPixelNoise2) {}

        template<class T>
        bool operator()(const T *const r, const T *const t, T *residual) const {
            int type;
            if (is_same<T, double>::value) {
                type = CV_64F;
            } else if (is_same<T, float>::value) {
                type = CV_32F;
            } else {
                // Unsupported.
                return false;
            }

            // Recover rotation and translation from the state vector.
            Mat Rt(3, 3, type);    // Transposed rotation matrix.
            Rodrigues(Mat(1, 3, type, (void *) r), Rt);
            Mat(Rt.t()).convertTo(Rt, CV_32F);
            Mat tt(1, 3, type, (void *) t);  // Transposed translation matrix.
            tt.convertTo(tt, CV_32F);
            tt = repeat(tt, pReferenceKF->mHighGradPtDepth.rows, 1);

            // Calculate projected 2D location in the current frame
            // of the high gradient points in the reference keyframe.
            Mat proj3d = (repeat(pReferenceKF->mHighGradPtDepth, 1, 3)
                                  .mul(pReferenceKF->mHighGradPtHomo2dCoord)
                          * invKt * Rt + tt) * Kt;
            Mat proj2d = proj3d.colRange(0, 2) / repeat(proj3d.col(2), 1, 2);
            assert(proj2d.cols == 2);
            proj2d.convertTo(proj2d, CV_32S);

            // Also calculate projected 2D location using slightly adjusted depth map.
            Mat proj3dRight = (repeat(pReferenceKF->mHighGradPtDepth + pReferenceKF->mHighGradPtSqrtUncertainty, 1, 3)
                                       .mul(pReferenceKF->mHighGradPtHomo2dCoord) * invKt * Rt + tt) * Kt;
            Mat proj2dRight = proj3dRight.colRange(0, 2) / repeat(proj3dRight.col(2), 1, 2);
            proj2dRight.convertTo(proj2dRight, CV_32S);

            Mat valid = proj2d.col(0) >= 0;
            bitwise_and(valid, proj2d.col(0) < imColor.cols, valid, valid);
            bitwise_and(valid, proj2d.col(1) >= 0, valid, valid);
            bitwise_and(valid, proj2d.col(1) < imColor.rows, valid, valid);
            bitwise_and(valid, proj2dRight.col(0) >= 0, valid, valid);
            bitwise_and(valid, proj2dRight.col(0) < imColor.cols, valid, valid);
            bitwise_and(valid, proj2dRight.col(1) >= 0, valid, valid);
            bitwise_and(valid, proj2dRight.col(1) < imColor.rows, valid, valid);

            // Extract pixels from the current frame.
            Mat pixels(valid.rows, 3, CV_8U), pixelsRight(valid.rows, 3, CV_8U);
#pragma omp parallel for
            for (int i = 0; i < valid.rows; ++i) {
                if (valid.at<uchar>(i)) {
                    auto pixel = imColor.at<Vec3b>(proj2d.at<int>(i, 1), proj2d.at<int>(i, 0));
                    pixels.at<uchar>(i, 0) = pixel.val[0];
                    pixels.at<uchar>(i, 1) = pixel.val[1];
                    pixels.at<uchar>(i, 2) = pixel.val[2];

                    pixel = imColor.at<Vec3b>(proj2dRight.at<int>(i, 1), proj2dRight.at<int>(i, 0));
                    pixelsRight.at<uchar>(i, 0) = pixel.val[0];
                    pixelsRight.at<uchar>(i, 1) = pixel.val[1];
                    pixelsRight.at<uchar>(i, 2) = pixel.val[2];
                }
            }

            // Calculate photometric residual.
            Mat res = pReferenceKF->mHighGradPtPixels - pixels;
            res.convertTo(res, CV_32F);
            cv::pow(res, 2, res);
            reduce(res, res, 1, CV_REDUCE_SUM, CV_32F);
            cv::sqrt(res, res);
            Mat resRight = pReferenceKF->mHighGradPtPixels - pixelsRight;
            resRight.convertTo(resRight, CV_32F);
            cv::pow(resRight, 2, resRight);
            reduce(resRight, resRight, 1, CV_REDUCE_SUM, CV_32F);
            cv::sqrt(resRight, resRight);

            Mat diff;
            cv::pow(resRight - res, 2, diff);
            Mat var = diff + 2 * cameraPixelNoise2;
            cv::sqrt(var, var);
            Mat regRes = res / var;
            // Set the invalid points to mean residual.
            float meanRes = static_cast<float>(mean(regRes, valid)[0]);
#pragma omp parallel for
            for (int i = 0; i < valid.rows; ++i)
                if (!valid.at<uchar>(i))
                    regRes.at<float>(i) = meanRes;

            // Fill the residual.
            regRes.convertTo(regRes, type);
            assert(regRes.isContinuous());
            memcpy(residual, regRes.data, sizeof(T) * regRes.rows);

            cv::pow(regRes, 2, regRes);
            cout << RotationAngle(Rt.t()) << ' ' << Mat(1, 3, CV_64F, (void *) t) << ' ' << sum(regRes)[0] << endl;

            return true;
        }
    };

    float EstimateCameraPose(const Mat &imColor,
                             const Mat &K,
                             const Mat &invK,
                             ORB_SLAM2::KeyFrame *pRefKF,
                             float cameraPixelNoise2,
                             double max_seconds,
                             Mat &Tcw,
                             float *rotAngle,
                             float *transDist,
                             float *validRatio) {
        // Initialize pose as no-transform.
        // The pose is expressed as scale (1-dim, not used), rotation (3-dim rodrigues) and translation (3-dim).
        double relRotationRodrigues[] = {0, 0, 0};
        double relTranslation[] = {0, 0, 0};

        Problem problem;
        CostFunction *cost_function = new NumericDiffCostFunction<CostFunctor, RIDDERS, TRACKING_NUM_PT, 3, 3>(
                new CostFunctor(imColor, pRefKF, K, invK, cameraPixelNoise2));
        auto *loss_function = new LossFunctionWrapper(new HuberLoss(TRACKING_HUBER_DELTA), TAKE_OWNERSHIP);
        problem.AddResidualBlock(cost_function, loss_function, relRotationRodrigues, relTranslation);

        Solver::Options options;
        options.num_threads = thread::hardware_concurrency();   // Use all cores.
        options.max_solver_time_in_seconds = max_seconds; // Enforce real-time.
        Solver::Summary summary;
        cout << "Solving..." << endl;
        ceres::Solve(options, &problem, &summary);
        cout << "Solver finished with final cost " << summary.final_cost << "!" << endl;

        Mat Rrel;
        Rodrigues(Mat(1, 3, CV_64F, relRotationRodrigues), Rrel);
        Rrel.convertTo(Rrel, CV_32F);
        Mat Trel = Mat::zeros(4, 4, CV_32F);
        Rrel.copyTo(Trel.rowRange(0, 3).colRange(0, 3));
        Trel.at<float>(0, 3) = static_cast<float>(relTranslation[0]);
        Trel.at<float>(1, 3) = static_cast<float>(relTranslation[1]);
        Trel.at<float>(2, 3) = static_cast<float>(relTranslation[2]);
        Trel.at<float>(3, 3) = 1;

        Tcw = Trel * pRefKF->GetPose();
        if (rotAngle) *rotAngle = RotationAngle(Rrel);
        if (transDist) *transDist = TranslationDist(Trel.col(3).rowRange(0, 3));
        if (validRatio) {
            // Calculate projected 2D location in the current frame
            // of the high gradient points in the reference keyframe.
            Mat proj3d =
                    (repeat(pRefKF->mHighGradPtDepth, 1, 3).mul(pRefKF->mHighGradPtHomo2dCoord)
                     * pRefKF->mInvK.t() * Rrel.t() +
                     repeat(Trel.col(3).rowRange(0, 3).t(), pRefKF->mHighGradPtDepth.rows, 1))
                    * K;
            Mat proj2d = proj3d.colRange(0, 2) / repeat(proj3d.col(2), 1, 2);
            assert(proj2d.cols == 2);
            proj2d.convertTo(proj2d, CV_32S);

            Mat valid = proj2d.col(0) >= 0;
            bitwise_and(valid, proj2d.col(0) < imColor.rows, valid, valid);
            bitwise_and(valid, proj2d.col(1) >= 0, valid, valid);
            bitwise_and(valid, proj2d.col(1) < imColor.cols, valid, valid);
            *validRatio = static_cast<float>(sum(valid)[0] / 255 / valid.rows);
        }

        return static_cast<float>(summary.final_cost) / TRACKING_NUM_PT;
    }
}