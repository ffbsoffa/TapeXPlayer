// source_alt/core/display/metal_renderer.h

#ifndef METAL_RENDERER_H
#define METAL_RENDERER_H

// Only compile for Apple platforms
#ifdef __APPLE__

#include <SDL2/SDL.h>
#include <CoreVideo/CoreVideo.h> // For CVPixelBufferRef

// Forward declare Metal types using @class
@class CAMetalLayer;
@protocol MTLDevice;
@protocol MTLCommandQueue;
@protocol MTLRenderPipelineState;
@protocol MTLBuffer;
@protocol MTLTexture;

// Requires linking with Metal.framework and QuartzCore.framework
#import <QuartzCore/CAMetalLayer.h> // Import required for CAMetalLayer*

class MetalRenderer {
public:
    MetalRenderer();
    ~MetalRenderer();

    // Initialize the Metal renderer using an existing SDL_Renderer
    bool initialize(SDL_Renderer* sdlRenderer);

    // Render a CVPixelBufferRef using the provided SDL_Renderer's Metal context
    bool render(CVPixelBufferRef pixelBuffer, SDL_Renderer* sdlRenderer);

    // Clean up Metal resources
    void cleanup();

    // Check if initialized
    bool isInitialized() const { return initialized; }

private:
    // Metal specific objects (using Objective-C id type)
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLRenderPipelineState> renderPipelineState; // Renamed from pipelineState
    CVMetalTextureCacheRef textureCache; // CoreVideo Metal Texture Cache
    CAMetalLayer* layer; // Store the layer

    // Member variables for textures created from CVPixelBuffer
    id<MTLTexture> yTexture;      // For NV12 Y plane
    id<MTLTexture> cbcrTexture;   // For NV12 CbCr plane
    id<MTLTexture> plane0Texture; // For single-plane formats like BGRA

    bool initialized;

    // Helper methods
    bool setupRenderPipeline(); // Renamed from setupPipeline
    // bool createQuadVertexBuffer(); // Removed - vertices passed dynamically
    // New helper method to create/update textures from CVPixelBuffer
    bool createMetalTexturesFromBuffer(CVPixelBufferRef pixelBuffer);

};

#endif // __APPLE__
#endif // METAL_RENDERER_H