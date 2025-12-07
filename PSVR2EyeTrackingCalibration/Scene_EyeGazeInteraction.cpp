// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include <XrSceneLib/PbrModelObject.h>
#include <XrSceneLib/Scene.h>
#include <XrSceneLib/TextTexture.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <numeric>

// IPC & Path 相关头文件
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#pragma comment(lib, "ws2_32.lib")

using namespace DirectX;
using namespace xr::math;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {
    // 简易 IPC 协议定义
    enum ECommandType : uint16_t {
        Command_ClientStartGazeCalibration = 14,
        Command_ClientStopGazeCalibration = 15,
    };
    struct CommandHeader {
        uint16_t type;
        int32_t dataLen;
    };
    const int IPC_PORT = 3364;

    void SendIPC(uint16_t commandType) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            return;

        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock != INVALID_SOCKET) {
            sockaddr_in serverAddr;
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(IPC_PORT);
            inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

            if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) != SOCKET_ERROR) {
                CommandHeader header;
                header.type = commandType;
                header.dataLen = 0;
                send(sock, (char*)&header, sizeof(header), 0);
            }
            closesocket(sock);
        }
        WSACleanup();
    }

    std::string GetConfigPath() {
        char path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, path))) {
            return std::string(path) + "\\PSVR2Calibration.txt";
        }
        return "C:\\PSVR2Calibration.txt";
    }

    struct CalibrationPoint {
        XrVector3f localPosition;
    };

    struct RecordedData {
        XrVector3f targetDir;
        XrVector3f gazeDir;
    };

    enum class AppState { Intro, Calibrating, Finished };

    struct EyeGazeInteractionScene : public engine::Scene {
        EyeGazeInteractionScene(engine::Context& context)
            : Scene(context)
            , m_supportsEyeGazeAction(context.Extensions.SupportsEyeGazeInteraction &&
                                      context.System.EyeGazeInteractionProperties.supportsEyeGazeInteraction) {
            SendIPC(Command_ClientStartGazeCalibration);

            sample::ActionSet& actionSet = ActionContext().CreateActionSet("calibration_actions", "Calibration Actions");

            m_gazeAction = actionSet.CreateAction("gaze_action", "Gaze Action", XR_ACTION_TYPE_POSE_INPUT, {});
            m_selectAction = actionSet.CreateAction("select_action", "Confirm Action", XR_ACTION_TYPE_BOOLEAN_INPUT, {});

            ActionContext().SuggestInteractionProfileBindings("/interaction_profiles/ext/eye_gaze_interaction",
                                                              {{m_gazeAction, "/user/eyes_ext/input/gaze_ext/pose"}});

            ActionContext().SuggestInteractionProfileBindings(
                "/interaction_profiles/khr/simple_controller",
                {{m_selectAction, "/user/hand/right/input/select/click"}, {m_selectAction, "/user/hand/left/input/select/click"}});
            ActionContext().SuggestInteractionProfileBindings("/interaction_profiles/oculus/touch_controller",
                                                              {{m_selectAction, "/user/hand/right/input/trigger/value"}});

            if (m_supportsEyeGazeAction) {
                XrActionSpaceCreateInfo createInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                createInfo.action = m_gazeAction;
                createInfo.poseInActionSpace = Pose::Identity();
                CHECK_XRCMD(xrCreateActionSpace(m_context.Session.Handle, &createInfo, m_gazeSpace.Put(xrDestroySpace)));
            }

            {
                XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                createInfo.poseInReferenceSpace = Pose::Identity();
                CHECK_XRCMD(xrCreateReferenceSpace(m_context.Session.Handle, &createInfo, m_viewSpace.Put(xrDestroySpace)));
            }

            // 3. 定义 9 个校准点 (Head-Locked, Z = -2.0m)
            // 进行了人体工程学调整：上方幅度减小，下方幅度增大

            // Y轴参数 (非对称)
            float upY = 0.4f;    // 之前是 0.5，稍微降低一点，减少抬眼压力
            float downY = -0.7f; // 之前是 -0.5，加深一点，因为往下看更容易

            // X轴参数 (对称)
            float sideX = 0.6f; // 稍微宽一点

            // 四角参数 (稍微内缩，避免极值)
            float cornerX = 0.5f;
            float cornerUpY = 0.35f;
            float cornerDownY = -0.6f;

            // 0. 中心
            m_calibrationPoints.push_back({{0.0f, 0.0f, -2.0f}});

            // 十字方向
            m_calibrationPoints.push_back({{0.0f, upY, -2.0f}});    // 上
            m_calibrationPoints.push_back({{0.0f, downY, -2.0f}});  // 下
            m_calibrationPoints.push_back({{-sideX, 0.0f, -2.0f}}); // 左
            m_calibrationPoints.push_back({{sideX, 0.0f, -2.0f}});  // 右

            // 四个角落
            m_calibrationPoints.push_back({{-cornerX, cornerUpY, -2.0f}});   // 左上
            m_calibrationPoints.push_back({{cornerX, cornerUpY, -2.0f}});    // 右上
            m_calibrationPoints.push_back({{-cornerX, cornerDownY, -2.0f}}); // 左下
            m_calibrationPoints.push_back({{cornerX, cornerDownY, -2.0f}});  // 右下

            // 4. 创建校准目标 (红色球体)
            m_targetObject = AddObject(engine::CreateSphere(m_context.PbrResources, 0.02f, 40, {1.0f, 0.0f, 0.0f, 1.0f}, 0.8f, 0.1f));
            m_targetObject->SetVisible(false);

            // 5. 创建文字
            engine::TextTextureInfo textInfo(1024, 512);
            textInfo.Foreground = {1.0f, 1.0f, 1.0f, 1.0f};
            textInfo.Background = {0.0f, 0.0f, 0.0f, 0.0f};
            textInfo.FontSize = 48.0f;
            textInfo.Margin = 10.0f;
            textInfo.TextAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
            textInfo.ParagraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

            m_textTexture = std::make_shared<engine::TextTexture>(m_context, textInfo);
            auto material = m_textTexture->CreatePbrMaterial(m_context.PbrResources);
            material->SetAlphaBlended(true);

            auto quadObject = engine::CreateQuad(m_context.PbrResources, {1.5f, 0.75f}, material);
            m_textObject = AddObject(quadObject);

            SetState(AppState::Intro);
        }

        void SetState(AppState newState) {
            m_appState = newState;

            switch (m_appState) {
            case AppState::Intro:
                m_textObject->SetVisible(true);
                m_targetObject->SetVisible(false);
                // 提示改为 9 点校准
                m_textTexture->Draw(
                    "PSVR2 Eye Tracking Calibration\n\nLook at the red dot\nand pull the RIGHT trigger.\n\nPull trigger to START.");
                break;

            case AppState::Calibrating:
                m_textObject->SetVisible(false);
                m_targetObject->SetVisible(true);
                break;

            case AppState::Finished:
                m_textObject->SetVisible(true);
                m_targetObject->SetVisible(false);
                m_textTexture->Draw("Calibration Complete!\nData saved to Documents.\n\nPull trigger to EXIT.");
                break;
            }
        }

        void OnUpdate(const engine::FrameTime& frameTime) override {
            XrSpaceLocation viewInApp{XR_TYPE_SPACE_LOCATION};
            CHECK_XRCMD(xrLocateSpace(m_viewSpace.Get(), m_context.AppSpace, frameTime.PredictedDisplayTime, &viewInApp));

            XrSpaceLocation gazeInView{XR_TYPE_SPACE_LOCATION};
            if (m_gazeSpace) {
                CHECK_XRCMD(xrLocateSpace(m_gazeSpace.Get(), m_viewSpace.Get(), frameTime.PredictedDisplayTime, &gazeInView));
            }

            if (Pose::IsPoseValid(viewInApp)) {
                XMVECTOR headPos = LoadXrVector3(viewInApp.pose.position);
                XMVECTOR headRot = LoadXrQuaternion(viewInApp.pose.orientation);
                XMVECTOR forward = XMVectorSet(0, 0, -1, 0);

                if (m_textObject->IsVisible()) {
                    XMVECTOR offset = XMVectorScale(forward, 1.5f);
                    XMVECTOR finalPos = XMVectorAdd(headPos, XMVector3Rotate(offset, headRot));
                    XrVector3f pos;
                    StoreXrVector3(&pos, finalPos);
                    m_textObject->Pose().position = pos;
                    m_textObject->Pose().orientation = viewInApp.pose.orientation;
                }

                if (m_targetObject->IsVisible() && m_currentStep < m_calibrationPoints.size()) {
                    XMVECTOR localTargetPos = LoadXrVector3(m_calibrationPoints[m_currentStep].localPosition);
                    XMVECTOR worldTargetPos = XMVectorAdd(headPos, XMVector3Rotate(localTargetPos, headRot));
                    XrVector3f finalPos;
                    StoreXrVector3(&finalPos, worldTargetPos);
                    m_targetObject->Pose().position = finalPos;
                    m_targetObject->Pose().orientation = viewInApp.pose.orientation;
                }
            }

            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = m_selectAction;
            XrActionStateBoolean selectState{XR_TYPE_ACTION_STATE_BOOLEAN};
            xrGetActionStateBoolean(m_context.Session.Handle, &getInfo, &selectState);

            bool isTriggerPressed = selectState.isActive && selectState.currentState && selectState.changedSinceLastSync;

            if (m_appState == AppState::Intro) {
                if (isTriggerPressed) {
                    SetState(AppState::Calibrating);
                }
            } else if (m_appState == AppState::Calibrating) {
                if (isTriggerPressed && Pose::IsPoseValid(gazeInView)) {
                    RecordData(gazeInView.pose);
                    m_currentStep++;
                    if (m_currentStep >= m_calibrationPoints.size()) {
                        FinishCalibration();
                        SetState(AppState::Finished);
                    }
                }
            } else if (m_appState == AppState::Finished) {
                if (isTriggerPressed) {
                    exit(0);
                }
            }
        }

        void RecordData(const XrPosef& gazePoseInView) {
            RecordedData data;

            XMVECTOR targetPos = LoadXrVector3(m_calibrationPoints[m_currentStep].localPosition);
            XMVECTOR targetDir = XMVector3Normalize(targetPos);
            StoreXrVector3(&data.targetDir, targetDir);

            XMVECTOR gazeRot = LoadXrQuaternion(gazePoseInView.orientation);
            XMVECTOR forward = XMVectorSet(0, 0, -1, 0);
            XMVECTOR gazeDirVec = XMVector3Rotate(forward, gazeRot);
            StoreXrVector3(&data.gazeDir, gazeDirVec);

            m_recordedData.push_back(data);
        }

        void FinishCalibration() {
            float sumDiffX = 0.0f;
            float sumDiffY = 0.0f;

            for (const auto& data : m_recordedData) {
                sumDiffX += (data.targetDir.x - data.gazeDir.x);
                sumDiffY += (data.targetDir.y - data.gazeDir.y);
            }

            float avgOffsetX = sumDiffX / m_recordedData.size();
            float avgOffsetY = sumDiffY / m_recordedData.size();

            std::string path = GetConfigPath();
            std::ofstream outFile(path);
            if (outFile.is_open()) {
                outFile << avgOffsetX << " " << avgOffsetY << std::endl;
                outFile.close();
                SendIPC(Command_ClientStopGazeCalibration);
            }
        }

    private:
        const bool m_supportsEyeGazeAction{false};

        xr::SpaceHandle m_gazeSpace;
        xr::SpaceHandle m_viewSpace;

        XrAction m_gazeAction{XR_NULL_HANDLE};
        XrAction m_selectAction{XR_NULL_HANDLE};

        std::shared_ptr<engine::Object> m_targetObject;
        std::shared_ptr<engine::Object> m_textObject;
        std::shared_ptr<engine::TextTexture> m_textTexture;

        std::vector<CalibrationPoint> m_calibrationPoints;
        std::vector<RecordedData> m_recordedData;

        int m_currentStep = 0;
        AppState m_appState = AppState::Intro;
    };
} // namespace

std::unique_ptr<engine::Scene> TryCreateEyeGazeInteractionScene(engine::Context& context) {
    return std::make_unique<EyeGazeInteractionScene>(context);
}