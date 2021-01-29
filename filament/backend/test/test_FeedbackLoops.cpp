/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BackendTest.h"

#include <private/filament/EngineEnums.h>

#include "ShaderGenerator.h"
#include "TrianglePrimitive.h"

#include <utils/Hash.h>

#include <fstream>

#ifndef IOS
#include <imageio/ImageEncoder.h>
#include <image/ColorTransform.h>

using namespace image;
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// Shaders
////////////////////////////////////////////////////////////////////////////////////////////////////

static std::string fullscreenVs = R"(#version 450 core
layout(location = 0) in vec4 mesh_position;
void main() {
    // Hack: move and scale triangle so that it covers entire viewport.
    gl_Position = vec4((mesh_position.xy + 0.5) * 5.0, 0.0, 1.0);
})";

static std::string fullscreenFs = R"(#version 450 core
precision mediump int; precision highp float;
layout(location = 0) out vec4 fragColor;
layout(location = 0) uniform sampler2D tex;
uniform Params {
    highp float fbWidth;
    highp float fbHeight;
    highp float sourceLevel;
    highp float unused;
} params;
void main() {
    vec2 fbsize = vec2(params.fbWidth, params.fbHeight);
    vec2 uv = (gl_FragCoord.xy + 0.5) / fbsize;
    fragColor = textureLod(tex, uv, params.sourceLevel);
})";

static uint32_t goldenPixelHash = 0;

static constexpr int kTexWidth = 360;
static constexpr int kTexHeight = 375;
static constexpr int kNumLevels = 6;
static constexpr int kNumFrames = 2;

namespace test {

using namespace filament;
using namespace filament::backend;

struct MaterialParams {
    float fbWidth;
    float fbHeight;
    float sourceLevel;
    float unused;
};

static void uploadUniforms(DriverApi& dapi, Handle<HwUniformBuffer> ubh, MaterialParams params) {
    MaterialParams* tmp = new MaterialParams(params);
    auto cb = [](void* buffer, size_t size, void* user) {
        MaterialParams* sp = (MaterialParams*) buffer;
        delete sp;
    };
    BufferDescriptor bd(tmp, sizeof(MaterialParams), cb);
    dapi.loadUniformBuffer(ubh, std::move(bd));
}

static void dumpScreenshot(DriverApi& dapi, Handle<HwRenderTarget> rt) {
    const size_t size = kTexWidth * kTexHeight * 4;
    void* buffer = calloc(1, size);
    auto cb = [](void* buffer, size_t size, void* user) {
        int w = kTexWidth, h = kTexHeight;
        const uint32_t* texels = (uint32_t*) buffer;
        goldenPixelHash = utils::hash::murmur3(texels, size / 4, 0);
        #ifndef IOS
        LinearImage image(w, h, 4);
        image = toLinearWithAlpha<uint8_t>(w, h, w * 4, (uint8_t*) buffer);
        std::ofstream pngstrm("feedback.png", std::ios::binary | std::ios::trunc);
        ImageEncoder::encode(pngstrm, ImageEncoder::Format::PNG, image, "", "feedback.png");
        #endif
    };
    PixelBufferDescriptor pb(buffer, size, PixelDataFormat::RGBA, PixelDataType::UBYTE, cb);
    dapi.readPixels(rt, 0, 0, kTexWidth, kTexHeight, std::move(pb));
}

TEST_F(BackendTest, FeedbackLoops) {
    // The test is executed within this block scope to force destructors to run before
    // executeCommands().
    {
        // Create a platform-specific SwapChain and make it current.
        auto swapChain = createSwapChain();
        getDriverApi().makeCurrent(swapChain, swapChain);

        // Create programs.
        ProgramHandle program;
        {
            ShaderGenerator shaderGen(fullscreenVs, fullscreenFs, sBackend, sIsMobilePlatform);
            Program prog = shaderGen.getProgram();
            Program::Sampler psamplers[] = { utils::CString("tex"), 0, false };
            prog.setSamplerGroup(0, psamplers, sizeof(psamplers) / sizeof(psamplers[0]));
            prog.setUniformBlock(1, utils::CString("params"));
            program = getDriverApi().createProgram(std::move(prog));
        }

        TrianglePrimitive triangle(getDriverApi());

        // Create a texture.
        auto usage = TextureUsage::COLOR_ATTACHMENT | TextureUsage::SAMPLEABLE;
        Handle<HwTexture> texture = getDriverApi().createTexture(
                    SamplerType::SAMPLER_2D,            // target
                    kNumLevels,                         // levels
                    TextureFormat::R11F_G11F_B10F,      // format
                    1,                                  // samples
                    kTexWidth,                          // width
                    kTexHeight,                         // height
                    1,                                  // depth
                    usage);                             // usage

        // Create a RenderTarget for each miplevel.
        Handle<HwRenderTarget> renderTargets[kNumLevels];
        for (uint8_t level = 0; level < kNumLevels; level++) {
            printf("Level %d: %d x %d\n", level, kTexWidth >> level, kTexHeight >> level);
            renderTargets[level] = getDriverApi().createRenderTarget(
                    TargetBufferFlags::COLOR,
                    kTexWidth >> level,                 // width of miplevel
                    kTexHeight >> level,                // height of miplevel
                    1,                                  // samples
                    { texture, level, 0 },              // color level
                    {},                                 // depth
                    {});                                // stencil
        }

        // Fill the base level of the texture with interesting colors.
        const size_t size = kTexHeight * kTexWidth * 4;
        uint8_t* buffer = (uint8_t*) malloc(size);
        for (int r = 0, i = 0; r < kTexHeight; r++) {
            for (int c = 0; c < kTexWidth; c++, i += 4) {
                buffer[i + 0] = 0x10;
                buffer[i + 1] = 0x1f * r / (kTexHeight - 1);
                buffer[i + 2] = 0x1f * c / (kTexWidth - 1);
                buffer[i + 3] = 0xf0;
            }
         }
        auto cb = [](void* buffer, size_t size, void* user) { free(buffer); };
        PixelBufferDescriptor pb(buffer, size, PixelDataFormat::RGBA, PixelDataType::UBYTE, cb);
        getDriverApi().update2DImage(texture, 0, 0, 0, kTexWidth, kTexHeight, std::move(pb));

        for (int frame = 0; frame < kNumFrames; frame++) {

            // Prep for rendering.
            RenderPassParams params = {};
            params.flags.clear = TargetBufferFlags::NONE;
            params.flags.discardEnd = TargetBufferFlags::NONE;
            PipelineState state;
            state.rasterState.colorWrite = true;
            state.rasterState.depthWrite = false;
            state.rasterState.depthFunc = RasterState::DepthFunc::A;
            state.program = program;
            backend::SamplerGroup samplers(1);
            backend::SamplerParams sparams = {};
            sparams.filterMag = SamplerMagFilter::LINEAR;
            sparams.filterMin = SamplerMinFilter::LINEAR_MIPMAP_NEAREST;
            samplers.setSampler(0, texture, sparams);
            auto sgroup = getDriverApi().createSamplerGroup(samplers.getSize());
            getDriverApi().updateSamplerGroup(sgroup, std::move(samplers.toCommandStream()));
            auto ubuffer = getDriverApi().createUniformBuffer(sizeof(MaterialParams),
                    backend::BufferUsage::STATIC);
            getDriverApi().makeCurrent(swapChain, swapChain);
            getDriverApi().beginFrame(0, 0);
            getDriverApi().bindSamplers(0, sgroup);    
            getDriverApi().bindUniformBuffer(0, ubuffer);

            // Downsample passes
            params.flags.discardStart = TargetBufferFlags::ALL;
            state.rasterState.disableBlending();
            for (int targetLevel = 1; targetLevel < kNumLevels; targetLevel++) {
                params.viewport.width = kTexWidth >> targetLevel;
                params.viewport.height = kTexHeight >> targetLevel;
                uploadUniforms(getDriverApi(), ubuffer, {
                    .fbWidth = float(params.viewport.width),
                    .fbHeight = float(params.viewport.height),
                    .sourceLevel = float(targetLevel - 1),
                });
                // getDriverApi().setMinMaxLevels(texture, 0, 0);
                getDriverApi().beginRenderPass(renderTargets[targetLevel], params);
                getDriverApi().draw(state, triangle.getRenderPrimitive());
                getDriverApi().endRenderPass();
            }

            // Upsample passes
            params.flags.discardStart = TargetBufferFlags::NONE;
            state.rasterState.blendFunctionSrcRGB = BlendFunction::ONE;
            state.rasterState.blendFunctionDstRGB = BlendFunction::ONE;
            for (int targetLevel = kNumLevels - 2; targetLevel >= 0; targetLevel--) {
                params.viewport.width = kTexWidth >> targetLevel;
                params.viewport.height = kTexHeight >> targetLevel;
                uploadUniforms(getDriverApi(), ubuffer, {
                    .fbWidth = float(params.viewport.width),
                    .fbHeight = float(params.viewport.height),
                    .sourceLevel = float(targetLevel + 1),
                });
                // getDriverApi().setMinMaxLevels(texture, 1, 1);
                getDriverApi().beginRenderPass(renderTargets[targetLevel], params);
                getDriverApi().draw(state, triangle.getRenderPrimitive());
                getDriverApi().endRenderPass();
            }

            // Read back the render target corresponding to the base level.
            //
            // NOTE: Calling glReadPixels on any miplevel other than the base level
            // seems to be un-reliable on some GPU's.
            if (frame == kNumFrames - 1) {
                dumpScreenshot(getDriverApi(), renderTargets[0]);
            }

            getDriverApi().flush();
            getDriverApi().commit(swapChain);
            getDriverApi().endFrame(0);
            getDriverApi().finish();
            executeCommands();
            getDriver().purge();
        }

        getDriverApi().destroyProgram(program);
        getDriverApi().destroySwapChain(swapChain);
        for (auto rt : renderTargets)  getDriverApi().destroyRenderTarget(rt);
    }

    const uint32_t expected = 0xfc0fac30;
    printf("Computed hash is 0x%8.8x, Expected 0x%8.8x\n", goldenPixelHash, expected);
    EXPECT_TRUE(goldenPixelHash == expected);
}

} // namespace test
