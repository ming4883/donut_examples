/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/Scene.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <taskflow/taskflow.hpp>
#include <nvrhi/utils.h>

using namespace donut;
using namespace donut::math;

static const char* g_WindowTitle = "Parallax Shadow Correction";

class ExampleForwardShadingPass : public render::ForwardShadingPass
{
public:
    ExampleForwardShadingPass(
            nvrhi::IDevice* device,
            std::shared_ptr<engine::CommonRenderPasses> commonPasses,
            bool isShadowmapPass) :
    ForwardShadingPass(
        device,
        commonPasses)
    {
        m_IsShadowPass = isShadowmapPass;
    }
    
protected:
    bool m_IsShadowPass = false;
    
    virtual nvrhi::ShaderHandle CreateVertexShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params)
    {
        if (m_IsShadowPass)
        {
            return shaderFactory.CreateShader("km/shadowdepth_vs.hlsl", "main", nullptr, nvrhi::ShaderType::Vertex);
        }
        else
        {
            return shaderFactory.CreateShader("km/forward_vs.hlsl", "main", nullptr, nvrhi::ShaderType::Vertex);
        }
    }
    virtual nvrhi::ShaderHandle CreatePixelShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool transmissiveMaterial)
    {
        std::vector<engine::ShaderMacro> Macros;
        Macros.push_back(engine::ShaderMacro("TRANSMISSIVE_MATERIAL", transmissiveMaterial ? "1" : "0"));

        if (m_IsShadowPass)
        {
            return shaderFactory.CreateShader("km/shadowdepth_ps.hlsl", "main", &Macros, nvrhi::ShaderType::Pixel);
        }
        else
        {
            
            return shaderFactory.CreateShader("km/forward_ps.hlsl", "main", &Macros, nvrhi::ShaderType::Pixel);   
        }
    }
};

class ExampleApp : public app::ApplicationBase
{
private:
    std::shared_ptr<vfs::RootFileSystem> m_RootFS;

    nvrhi::CommandListHandle m_CommandList;

    std::unique_ptr<tf::Executor> m_Executor;
    
    std::shared_ptr<engine::FramebufferFactory> m_SceneFramebuffer;
    std::shared_ptr<engine::FramebufferFactory> m_ShadowmapFramebuffer;
    
    std::unique_ptr<ExampleForwardShadingPass> m_ScenePass;
    std::unique_ptr<ExampleForwardShadingPass> m_ShadowmapPass;
    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;
    std::unique_ptr<engine::Scene> m_Scene;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

    app::FirstPersonCamera m_SceneCamera;
    engine::PlanarView m_SceneView;

    app::FirstPersonCamera m_ShadowmapCamera;
    engine::PlanarView m_ShadowmapView;

    uint32_t m_shadowMapSize = 512;

public:
    using ApplicationBase::ApplicationBase;
    
    bool Init()
    {
        std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/glTF-Sample-Models/2.0/GearboxAssy/glTF/GearboxAssy.gltf";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/parallax_shadow_correction" /  app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        
        m_RootFS = std::make_shared<vfs::RootFileSystem>();
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);
        m_RootFS->mount("/shaders/km", appShaderPath);

        m_Executor = std::make_unique<tf::Executor>();

        m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_RootFS, "/shaders");
        m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
        m_TextureCache = std::make_shared<engine::TextureCache>(GetDevice(), nativeFS, nullptr);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFileName);

        m_Scene->FinishedLoading(GetFrameIndex());
        auto aabb = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();
        m_SceneCamera.LookAt((aabb.m_maxs - aabb.center())* 2.0f + aabb.center(), aabb.center());
        m_SceneCamera.SetMoveSpeed(length(aabb.m_maxs - aabb.m_mins) * 0.1f);
        
        m_CommandList = GetDevice()->createCommandList();

        m_ScenePass = std::make_unique<ExampleForwardShadingPass>(GetDevice(), m_CommonPasses, false);
        m_ShadowmapPass = std::make_unique<ExampleForwardShadingPass>(GetDevice(), m_CommonPasses, true);
        render::ForwardShadingPass::CreateParameters forwardParams;
        forwardParams.numConstantBufferVersions = 128;
        m_ScenePass->Init(*m_ShaderFactory, forwardParams);
        m_ShadowmapPass->Init(*m_ShaderFactory, forwardParams);
        return true;
    }

    bool LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName) override 
    {
        engine::Scene* scene = new engine::Scene(GetDevice(), *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

        if (scene->LoadWithExecutor(sceneFileName, m_Executor.get()))
        {
            m_Scene = std::unique_ptr<engine::Scene>(scene);
            return true;
        }

        return false;
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        m_SceneCamera.KeyboardUpdate(key, scancode, action, mods);

        if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
        {
            //m_UseThreads = !m_UseThreads;
        }

        return true;
    }

    bool MousePosUpdate(double xpos, double ypos) override
    {
        m_SceneCamera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override
    {
        m_SceneCamera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        m_SceneCamera.Animate(fElapsedTimeSeconds);

        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle,"NONE");
    }

    void BackBufferResizing() override
    { 
        m_BindingCache->Clear();
        m_SceneFramebuffer = nullptr;
    }

    void EnsureFramebuffers(const nvrhi::FramebufferInfo& fbinfo)
    {
        if (!m_SceneFramebuffer)
        {
            auto textureDesc = nvrhi::TextureDesc()
                .setDimension(nvrhi::TextureDimension::Texture2D)
                .setWidth(fbinfo.width)
                .setHeight(fbinfo.height)
                .setClearValue(nvrhi::Color(0.f))
                .setIsRenderTarget(true)
                .setKeepInitialState(true);

            auto colorBuffer = GetDevice()->createTexture(textureDesc
                .setDebugName("SceneColorBuffer")
                .setFormat(nvrhi::Format::SRGBA8_UNORM)
                .setInitialState(nvrhi::ResourceStates::RenderTarget));

            auto depthBuffer = GetDevice()->createTexture(textureDesc
                .setDebugName("SceneDepthBuffer")
                .setFormat(nvrhi::Format::D32)
                .setInitialState(nvrhi::ResourceStates::DepthWrite));

            m_SceneFramebuffer = std::make_unique<engine::FramebufferFactory>(GetDevice());
            m_SceneFramebuffer->RenderTargets.push_back(colorBuffer);
            m_SceneFramebuffer->DepthTarget = depthBuffer;
        }

        if (!m_ShadowmapFramebuffer)
        {
            auto textureDesc = nvrhi::TextureDesc()
                .setDimension(nvrhi::TextureDimension::Texture2D)
                .setWidth(m_shadowMapSize)
                .setHeight(m_shadowMapSize)
                .setClearValue(nvrhi::Color(0.f))
                .setIsRenderTarget(true)
                .setKeepInitialState(true);

            auto colorBuffer = GetDevice()->createTexture(textureDesc
                .setDebugName("ShadowMapColorBuffer")
                .setFormat(nvrhi::Format::R32_FLOAT)
                .setInitialState(nvrhi::ResourceStates::RenderTarget));

            auto depthBuffer = GetDevice()->createTexture(textureDesc
                .setDebugName("ShadowMapDepthBuffer")
                .setFormat(nvrhi::Format::D32)
                .setInitialState(nvrhi::ResourceStates::DepthWrite));

            m_ShadowmapFramebuffer = std::make_unique<engine::FramebufferFactory>(GetDevice());
            m_ShadowmapFramebuffer->RenderTargets.push_back(colorBuffer);
            m_ShadowmapFramebuffer->DepthTarget = depthBuffer;
        }
    }

    void SetupShadowMapView(const donut::math::float3& lightDir)
    {
        auto aabb = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();
        auto lookat = aabb.center();

        float r_max = 0;
        for(uint32_t i = 0; i < 8; ++i)
        {
            float r = length(aabb.getCorner(i) - lookat);
            r_max = donut::math::max(r, r_max);
        }

        auto eyeat = lookat + lightDir * r_max;
        m_ShadowmapCamera.LookAt(eyeat, lookat, m_SceneCamera.GetUp());

        m_ShadowmapView.SetViewport(nvrhi::Viewport(static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize)));
        m_ShadowmapView.SetMatrices(m_ShadowmapCamera.GetWorldToViewMatrix(), orthoProjD3DStyle(-r_max, r_max, r_max, -r_max, 0.0, 2 * r_max));
        m_ShadowmapView.UpdateCache();
    }

    void RenderShadowMapView(nvrhi::ICommandList* commandList)
    {
        const float3 lightDir(0, 1, 0);
        SetupShadowMapView(lightDir);

        render::InstancedOpaqueDrawStrategy strategy;

        commandList->clearDepthStencilTexture(m_ShadowmapFramebuffer->DepthTarget, nvrhi::AllSubresources, true, 1.f, true, 0);
        commandList->clearTextureFloat(m_ShadowmapFramebuffer->RenderTargets[0], nvrhi::AllSubresources, nvrhi::Color(1.f));

        render::ForwardShadingPass::Context context;
        m_ShadowmapPass->PrepareLights(context, commandList, {}, 1.0f, 0.3f, {});

        render::RenderCompositeView(commandList, &m_ShadowmapView, &m_ShadowmapView, *m_ShadowmapFramebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(), strategy, *m_ShadowmapPass, context);
    }

    void RenderSceneView(nvrhi::ICommandList* commandList, const nvrhi::Viewport& viewport)
    {
        render::InstancedOpaqueDrawStrategy strategy;

        m_SceneView.SetViewport(viewport);
        m_SceneView.SetMatrices(m_SceneCamera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, viewport.width() / viewport.height(), 0.1f));
        m_SceneView.UpdateCache();

        commandList->clearDepthStencilTexture(m_SceneFramebuffer->DepthTarget, nvrhi::AllSubresources, true, 0.f, true, 0);
        commandList->clearTextureFloat(m_SceneFramebuffer->RenderTargets[0], nvrhi::AllSubresources, nvrhi::Color(0.f));

        render::ForwardShadingPass::Context context;
        m_ScenePass->PrepareLights(context, commandList, {}, 1.0f, 0.3f, {});
        render::RenderCompositeView(commandList, &m_SceneView, &m_SceneView, *m_SceneFramebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(), strategy, *m_ScenePass, context);
   
    }
    
    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        /*
        dm::affine viewMatrix = m_Camera.GetWorldToViewMatrix();
        m_CubemapView.SetTransform(viewMatrix, 0.1f, 100.f);
        m_CubemapView.UpdateCache();

        tf::Taskflow taskFlow;
        if (m_UseThreads)
        {
            for (int face = 0; face < 6; face++)
            {
                taskFlow.emplace([this, face]() { RenderCubeFace(face); });
            }

            m_Executor->run(taskFlow);
        }
        else
        {
            for (int face = 0; face < 6; face++)
            {
                RenderCubeFace(face);
            }
        }
        
        m_CommandList->open();

        const std::vector<std::pair<int, int>> faceLayout = {
            { 3, 1 },
            { 1, 1 },
            { 2, 0 },
            { 2, 2 },
            { 2, 1 },
            { 0, 1 }
        };

        auto fbinfo = framebuffer->getFramebufferInfo();
        int faceSize = std::min(fbinfo.width / 4, fbinfo.height / 3);

        for (int face = 0; face < 6; face++)
        {
            nvrhi::Viewport viewport;
            viewport.minX = float(faceLayout[face].first * faceSize);
            viewport.maxX = viewport.minX + float(faceSize);
            viewport.minY = float(faceLayout[face].second * faceSize);
            viewport.maxY = viewport.minY + float(faceSize);
            viewport.minZ = 0.f;
            viewport.maxZ = 1.f;

            engine::BlitParameters blitParams;
            blitParams.targetFramebuffer = framebuffer;
            blitParams.targetViewport = viewport;
            blitParams.sourceTexture = m_ColorBuffer;
            blitParams.sourceArraySlice = face;
            m_CommonPasses->BlitTexture(m_CommandList, blitParams, m_BindingCache.get());
        }
        
        m_CommandList->close();

        if (m_UseThreads)
        {
            m_Executor->wait_for_all();
        }

        nvrhi::ICommandList* commandLists[] = {
            m_FaceCommandLists[0],
            m_FaceCommandLists[1],
            m_FaceCommandLists[2],
            m_FaceCommandLists[3],
            m_FaceCommandLists[4],
            m_FaceCommandLists[5],
            m_CommandList
        };
        
        GetDevice()->executeCommandLists(commandLists, std::size(commandLists));
        */

        const auto& fbinfo = framebuffer->getFramebufferInfo();

        EnsureFramebuffers(fbinfo);

        nvrhi::ICommandList* commandList = m_CommandList;
        commandList->open();

        RenderShadowMapView(commandList);

        RenderSceneView(commandList, nvrhi::Viewport(float(fbinfo.width), float(fbinfo.height)));

        m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_SceneFramebuffer->RenderTargets[0], m_BindingCache.get());
        commandList->close();
        GetDevice()->executeCommandList(commandList);
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    if (api == nvrhi::GraphicsAPI::D3D11)
    {
        log::error("The Threaded Rendering example does not support D3D11.");
        return 1;
    }

    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 1024; // window size matches the layout of the rendered cube faces
    deviceParams.backBufferHeight = 768;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true; 
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }
    
    {
        ExampleApp example(deviceManager);
        if (example.Init())
        {
            deviceManager->AddRenderPassToBack(&example);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&example);
        }
    }
    
    deviceManager->Shutdown();

    delete deviceManager;

    return 0;
}
