// Linux port: VDXR's BodyStateV2 (hands, aim, eyes, face, body) filled from the headset's FrameData, which carries
// the same structures (offsets: tools/dcgen --layout VirtualDesktop.Mobile.FrameData).
#include "pch.h"

#include "BodyState.h"

namespace opendesk_openxr {
    using namespace BodyTracking;

    namespace {
        constexpr size_t kHandsActive = 312, kHandJoints[2] = {316, 1772}, kAim[2] = {3232, 3288};
        constexpr size_t kBodyCalibrated = 3344, kBodyHighFidelity = 3345, kBodyConfidence = 3348, kBodyJoints = 3352,
                         kBodyJointsBytes = 1280;
        constexpr size_t kEyeValid = 4632, kEyePose[2] = {4636, 4664}, kEyeConfidence[2] = {4692, 4696};
        constexpr size_t kFaceValid = 4700, kFaceEyeFollowingValid = 4701, kFaceWeights = 4704, kFaceConfidences = 4984;

        template <typename T>
        void read(const uint8_t* fd, size_t offset, T& out) {
            std::memcpy(&out, fd + offset, sizeof(T));
        }

        struct FrameBodyJoint { // FrameData body joint: u64 flags + pose, 8-byte aligned (40 bytes)
            uint64_t LocationFlags;
            Pose Pose;
        };
    } // namespace

    void FillBodyStateFromFrameData(const uint8_t* fd, BodyStateV2& s) {
        s.LeftHandActive = fd[kHandsActive];
        s.RightHandActive = fd[kHandsActive + 1];
        static_assert(sizeof(FingerJointState) * HandJointCount == 1456);
        std::memcpy(s.LeftHandJointStates, fd + kHandJoints[0], sizeof(s.LeftHandJointStates));
        std::memcpy(s.RightHandJointStates, fd + kHandJoints[1], sizeof(s.RightHandJointStates));
        static_assert(sizeof(HandTrackingAimState) == 56);
        std::memcpy(&s.LeftAimState, fd + kAim[0], sizeof(s.LeftAimState));
        std::memcpy(&s.RightAimState, fd + kAim[1], sizeof(s.RightAimState));

        s.BodyTrackingCalibrated = fd[kBodyCalibrated];
        s.BodyTrackingHighFidelity = fd[kBodyHighFidelity];
        read(fd, kBodyConfidence, s.BodyTrackingConfidence);
        // the headset sends 32 of XR_META_body_tracking_full_body's 84 joints (Xenko.VR OpenXRHMD.GetBodyJoints): slots
        // 0-17 are joints 0-17 (root to the arms), slots 18-31 the lower body, joints 70-83 (full body tracking only).
        // The hand joints in between (18-69) are not sent: hand tracking has them.
        for (auto& joint : s.BodyJoints) {
            joint.LocationFlags = 0;
        }
        const size_t slots = kBodyJointsBytes / sizeof(FrameBodyJoint);
        for (size_t i = 0; i < slots; i++) {
            const size_t joint = i <= 17 ? i : i + 52;
            if (joint >= FullBodyJointCount) {
                break;
            }
            FrameBodyJoint j;
            read(fd, kBodyJoints + i * sizeof(FrameBodyJoint), j);
            s.BodyJoints[joint].LocationFlags = j.LocationFlags;
            s.BodyJoints[joint].Pose = j.Pose;
        }
        // the hands (XR_BODY_JOINT_LEFT_HAND_PALM_FB = 18 ... RIGHT_HAND_LITTLE_TIP_FB = 69: XR_EXT_hand_tracking order) from
        // hand tracking while it tracks them
        if (s.BodyTrackingConfidence > 0.f) {
            const FingerJointState* hands[2] = {s.LeftHandJointStates, s.RightHandJointStates};
            const bool active[2] = {s.LeftHandActive != 0, s.RightHandActive != 0};
            for (int h = 0; h < 2; h++) {
                if (!active[h]) {
                    continue;
                }
                for (int i = 0; i < HandJointCount; i++) {
                    BodyJointLocation& joint = s.BodyJoints[18 + h * HandJointCount + i];
                    joint.LocationFlags = s.BodyJoints[0].LocationFlags; // tracked like the body
                    joint.Pose = hands[h][i].Pose;
                }
            }
        }

        s.LeftEyeIsValid = fd[kEyeValid];
        s.RightEyeIsValid = fd[kEyeValid + 1];
        read(fd, kEyePose[0], s.LeftEyePose);
        read(fd, kEyePose[1], s.RightEyePose);
        read(fd, kEyeConfidence[0], s.LeftEyeConfidence);
        read(fd, kEyeConfidence[1], s.RightEyeConfidence);

        s.FaceIsValid = fd[kFaceValid];
        s.IsEyeFollowingBlendshapesValid = fd[kFaceEyeFollowingValid];
        read(fd, kFaceWeights, s.ExpressionWeights);
        read(fd, kFaceConfidences, s.ExpressionConfidences);
    }
} // namespace opendesk_openxr
