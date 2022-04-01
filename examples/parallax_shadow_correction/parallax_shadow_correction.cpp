
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

#include "parallax_shadow_correction_cb.h"

static const char* g_WindowTitle = "Parallax Shadow Correction";

class ExampleForwardShadingPass : public render::ForwardShadingPass
{
    typedef ForwardShadingPass Super;
public:
    ExampleForwardShadingPass(
            nvrhi::IDevice* device,
            std::shared_ptr<engine::CommonRenderPasses> commonPasses,
            bool isShadowmapPass) :
    Super(
        device,
        commonPasses)
    {
        m_IsShadowPass = isShadowmapPass;
    }

    virtual void Init(
        engine::ShaderFactory& shaderFactory,
        const CreateParameters& params) override
    {
        if (!m_IsShadowPass)
            m_ParallaxShadowCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ParallaxShadowCorrectionConstants), "ParallaxShadowCorrectionConstants", params.numConstantBufferVersions));
        Super::Init(shaderFactory, params);
    }
    
    void PreparegParallaxShadow(
        Context& context,
        nvrhi::ICommandList* commandList,
        const ParallaxShadowCorrectionConstants& constants,
        nvrhi::ITexture* shadowMapTexture)
    {
        if (!m_IsShadowPass)
        {
            commandList->writeBuffer(m_ParallaxShadowCB, &constants, sizeof(constants));
            context.lightBindingSet = CreateLightBindingSet(shadowMapTexture, nullptr, nullptr, nullptr);
        }
    }
    
protected:
    bool m_IsShadowPass = false;
    nvrhi::BufferHandle m_ParallaxShadowCB;
    
    virtual nvrhi::ShaderHandle CreateVertexShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params) override
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
    virtual nvrhi::ShaderHandle CreatePixelShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool transmissiveMaterial) override
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


    virtual nvrhi::BindingLayoutHandle CreateViewBindingLayout() override
    {
        if (m_IsShadowPass)
            return Super::CreateViewBindingLayout();
        
        nvrhi::BindingLayoutDesc viewLayoutDesc;
        viewLayoutDesc.visibility = nvrhi::ShaderType::All;
        viewLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(3),
            nvrhi::BindingLayoutItem::Sampler(1)
        };

        return m_Device->createBindingLayout(viewLayoutDesc);
    }
    
    virtual nvrhi::BindingSetHandle CreateViewBindingSet() override
    {
        if (m_IsShadowPass)
            return Super::CreateViewBindingSet();
        
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(1, m_ForwardViewCB),
            nvrhi::BindingSetItem::ConstantBuffer(2, m_ForwardLightCB),
            nvrhi::BindingSetItem::ConstantBuffer(3, m_ParallaxShadowCB),
            nvrhi::BindingSetItem::Sampler(1, m_ShadowSampler)
        };
        bindingSetDesc.trackLiveness = m_TrackLiveness;

        return m_Device->createBindingSet(bindingSetDesc, m_ViewBindingLayout);
    }
};

class ExampleApp : public app::ApplicationBase
{
private:
    std::shared_ptr<vfs::RootFileSystem> m_RootFS;

    nvrhi::CommandListHandle m_CommandList;

    std::unique_ptr<tf::Executor> m_Executor;

    struct RenderPhase
    {
        std::shared_ptr<engine::FramebufferFactory> m_Framebuffer;
        std::unique_ptr<ExampleForwardShadingPass> m_GeomPass;
        app::FirstPersonCamera m_Camera;
        engine::PlanarView m_View;
    };
    
    RenderPhase m_MainPhase;
    RenderPhase m_ShadowmapPhase;
    
    uint32_t m_shadowMapSize = 1024;
    
    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;
    std::unique_ptr<engine::Scene> m_Scene;
    std::shared_ptr<engine::DirectionalLight> m_SunLight;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

public:
    using ApplicationBase::ApplicationBase;
    
    bool Init()
    {
        std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/glTF-Sample-Models/2.0/Buggy/glTF/Buggy.gltf";
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

        m_SunLight = std::make_shared<engine::DirectionalLight>();
        m_Scene->GetSceneGraph()->AttachLeafNode(m_Scene->GetSceneGraph()->GetRootNode(), m_SunLight);

        m_SunLight->SetDirection(double3(0.0, -1.0, 0.0));
        m_SunLight->irradiance = 1.f;

        m_Scene->FinishedLoading(GetFrameIndex());
        
        auto aabb = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();
        m_MainPhase.m_Camera.LookAt((aabb.m_maxs - aabb.center())* 2.0f + aabb.center(), aabb.center());
        m_MainPhase.m_Camera.SetMoveSpeed(length(aabb.m_maxs - aabb.m_mins) * 0.1f);

        render::ForwardShadingPass::CreateParameters forwardParams;
        forwardParams.numConstantBufferVersions = 128;
        
        m_MainPhase.m_GeomPass = std::make_unique<ExampleForwardShadingPass>(GetDevice(), m_CommonPasses, false);
        m_MainPhase.m_GeomPass->Init(*m_ShaderFactory, forwardParams);

        m_ShadowmapPhase.m_GeomPass = std::make_unique<ExampleForwardShadingPass>(GetDevice(), m_CommonPasses, true);
        m_ShadowmapPhase.m_GeomPass->Init(*m_ShaderFactory, forwardParams);
        
        m_CommandList = GetDevice()->createCommandList();

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
        m_MainPhase.m_Camera.KeyboardUpdate(key, scancode, action, mods);

        if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
        {
            //m_UseThreads = !m_UseThreads;
        }

        return true;
    }

    bool MousePosUpdate(double xpos, double ypos) override
    {
        m_MainPhase.m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override
    {
        m_MainPhase.m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        m_MainPhase.m_Camera.Animate(fElapsedTimeSeconds);

        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle,"NONE");
    }

    void BackBufferResizing() override
    { 
        m_BindingCache->Clear();
        m_MainPhase.m_Framebuffer = nullptr;
    }

    void EnsureFramebuffers(const nvrhi::FramebufferInfo& fbinfo)
    {
        if (!m_MainPhase.m_Framebuffer)
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

            m_MainPhase.m_Framebuffer = std::make_unique<engine::FramebufferFactory>(GetDevice());
            m_MainPhase.m_Framebuffer->RenderTargets.push_back(colorBuffer);
            m_MainPhase.m_Framebuffer->DepthTarget = depthBuffer;
        }

        if (!m_ShadowmapPhase.m_Framebuffer)
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

            m_ShadowmapPhase.m_Framebuffer = std::make_unique<engine::FramebufferFactory>(GetDevice());
            m_ShadowmapPhase.m_Framebuffer->RenderTargets.push_back(colorBuffer);
            m_ShadowmapPhase.m_Framebuffer->DepthTarget = depthBuffer;
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
        auto up = float3(0, 1, 0);
        if (abs(dot(up, normalize((lookat - eyeat)))) >= 1.0)
        {
            up = float3(1, 0, 0);
        }
        m_ShadowmapPhase.m_Camera.LookAt(eyeat, lookat, up);

        m_ShadowmapPhase.m_View.SetViewport(nvrhi::Viewport(static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize)));
        m_ShadowmapPhase.m_View.SetMatrices(m_ShadowmapPhase.m_Camera.GetWorldToViewMatrix(), orthoProjD3DStyle(-r_max, r_max, r_max, -r_max, 0.0, 2 * r_max));
        m_ShadowmapPhase.m_View.UpdateCache();
    }

    void RenderShadowMapView(nvrhi::ICommandList* commandList)
    {
        SetupShadowMapView(-float3(m_SunLight->GetDirection()));

        render::InstancedOpaqueDrawStrategy strategy;

        commandList->clearDepthStencilTexture(m_ShadowmapPhase.m_Framebuffer->DepthTarget, nvrhi::AllSubresources, true, 1.f, true, 0);
        commandList->clearTextureFloat(m_ShadowmapPhase.m_Framebuffer->RenderTargets[0], nvrhi::AllSubresources, nvrhi::Color(1.f));

        render::ForwardShadingPass::Context context;
        m_ShadowmapPhase.m_GeomPass->PrepareLights(context, commandList, {}, 0.0f, 0.0f, {});

        render::RenderCompositeView(commandList, &m_ShadowmapPhase.m_View, &m_ShadowmapPhase.m_View, *m_ShadowmapPhase.m_Framebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(), strategy, *m_ShadowmapPhase.m_GeomPass, context);
    }

    void RenderSceneView(nvrhi::ICommandList* commandList, const nvrhi::Viewport& viewport)
    {
        render::InstancedOpaqueDrawStrategy strategy;

        m_MainPhase.m_View.SetViewport(viewport);
        m_MainPhase.m_View.SetMatrices(m_MainPhase.m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, viewport.width() / viewport.height(), 0.1f));
        m_MainPhase.m_View.UpdateCache();

        commandList->clearDepthStencilTexture(m_MainPhase.m_Framebuffer->DepthTarget, nvrhi::AllSubresources, true, 0.f, true, 0);
        commandList->clearTextureFloat(m_MainPhase.m_Framebuffer->RenderTargets[0], nvrhi::AllSubresources, nvrhi::Color(0.f));

        render::ForwardShadingPass::Context context;
        m_MainPhase.m_GeomPass->PrepareLights(context, commandList, m_Scene->GetSceneGraph()->GetLights(), 0.125f, 0.0625f, {});
        
        ParallaxShadowCorrectionConstants parallaxConsts;
        parallaxConsts.cacheLightDir = float4(-float3(m_SunLight->GetDirection()), 0);
        parallaxConsts.frameLightDir = float4(-float3(m_SunLight->GetDirection()), 0);
        parallaxConsts.cacheWorldToShadow = m_ShadowmapPhase.m_View.GetViewProjectionMatrix();
        parallaxConsts.frameWorldToShadow = m_ShadowmapPhase.m_View.GetViewProjectionMatrix();
        
        m_MainPhase.m_GeomPass->PreparegParallaxShadow(context, commandList, parallaxConsts, m_ShadowmapPhase.m_Framebuffer->RenderTargets[0]);
        render::RenderCompositeView(commandList, &m_MainPhase.m_View, &m_MainPhase.m_View, *m_MainPhase.m_Framebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(), strategy, * m_MainPhase.m_GeomPass, context);
    }
    
    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        const auto& fbinfo = framebuffer->getFramebufferInfo();

        EnsureFramebuffers(fbinfo);

        nvrhi::ICommandList* commandList = m_CommandList;
        commandList->open();

        RenderShadowMapView(commandList);

        RenderSceneView(commandList, nvrhi::Viewport(float(fbinfo.width), float(fbinfo.height)));

        m_CommonPasses->BlitTexture(commandList, framebuffer, m_MainPhase.m_Framebuffer->RenderTargets[0], m_BindingCache.get());

        // display shadow map
        {
            nvrhi::Viewport viewport;
            viewport.minX = 0;
            viewport.maxX = 128;
            viewport.minY = static_cast<float>(fbinfo.height - 128);
            viewport.maxY = static_cast<float>(fbinfo.height);
            viewport.minZ = 0.f;
            viewport.maxZ = 1.f;

            engine::BlitParameters blitParams;
            blitParams.targetFramebuffer = framebuffer;
            blitParams.targetViewport = viewport;
            blitParams.sourceTexture = m_ShadowmapPhase.m_Framebuffer->RenderTargets[0];
            blitParams.sourceArraySlice = 0;
            m_CommonPasses->BlitTexture(commandList, blitParams, m_BindingCache.get());
        }
        
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
