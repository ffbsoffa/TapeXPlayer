// source_alt/core/display/metal_renderer.cpp

#ifdef __APPLE__ // Весь файл компилируется только на Apple

#include "metal_renderer.h"
#include <stdexcept> // Для исключений
#include <iostream>

// Подключаем полные заголовки Metal здесь
#import <Metal/Metal.h>
#import <CoreVideo/CVMetalTextureCache.h> // Для функций кэша
#import <SDL2/SDL_metal.h> // Для интеграции SDL и Metal

// Конструктор и деструктор
MetalRenderer::MetalRenderer() :
    device(nil),
    commandQueue(nil),
    renderPipelineState(nil),
    textureCache(nullptr), // Используем nullptr для C-указателей
    layer(nil),
    initialized(false)
{}

MetalRenderer::~MetalRenderer() {
    // Очистка должна быть вызвана явно, но на всякий случай
    cleanup();
}

// --- Реализация методов ---

bool MetalRenderer::initialize(SDL_Renderer* sdlRenderer) {
    std::cout << "[MetalRenderer] Initializing..." << std::endl;
#ifdef __APPLE__
    if (!sdlRenderer) {
        std::cerr << "[MetalRenderer] Error: SDL_Renderer is null." << std::endl;
        return false;
    }

    // Get CAMetalLayer from SDL Renderer
    layer = (CAMetalLayer*)SDL_RenderGetMetalLayer(sdlRenderer);
    if (!layer) {
         std::cerr << "[MetalRenderer] Error: Could not get CAMetalLayer from SDL_Renderer. Ensure renderer uses Metal driver." << std::endl;
         // You might need SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal"); before creating the renderer.
         return false;
    }

    // Get MTLDevice from the layer
    device = layer.device;
    if (!device) {
        std::cerr << "[MetalRenderer] Error: Could not get MTLDevice from CAMetalLayer." << std::endl;
        return false;
    }
    std::cout << "[MetalRenderer] Got MTLDevice: " << [[device name] UTF8String] << std::endl;

    // Create command queue
    commandQueue = [device newCommandQueue];
    if (!commandQueue) {
        std::cerr << "[MetalRenderer] Error: Could not create MTLCommandQueue." << std::endl;
        return false;
    }
    std::cout << "[MetalRenderer] Created MTLCommandQueue." << std::endl;

    // Create texture cache
    CVReturn err = CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &textureCache);
    if (err != kCVReturnSuccess) {
        std::cerr << "[MetalRenderer] Error: CVMetalTextureCacheCreate failed with error " << err << std::endl;
        return false;
    }
    std::cout << "[MetalRenderer] Created CVMetalTextureCache." << std::endl;

    // Create vertex buffer first, as pipeline setup might need its description
    // if (!createQuadVertexBuffer()) { // Removed
    //     std::cerr << "[MetalRenderer] Error: Failed to create vertex buffer." << std::endl;
    //     cleanup();
    //     return false;
    // }

    // Setup render pipeline state (now vertex buffer exists)
    if (!setupRenderPipeline()) {
        std::cerr << "[MetalRenderer] Error: Failed to setup render pipeline." << std::endl;
        cleanup(); // Release already created resources
        return false;
    }
     std::cout << "[MetalRenderer] Render pipeline setup successfully." << std::endl;

    initialized = true;
    std::cout << "[MetalRenderer] Initialization complete." << std::endl;
    return true;
#else
    return false; // Metal not supported
#endif // __APPLE__
}

void MetalRenderer::cleanup() {
    std::cout << "[MetalRenderer] Cleaning up..." << std::endl;
    // Освобождаем Obj-C объекты
    if (renderPipelineState) {
        [renderPipelineState release];
        renderPipelineState = nil;
    }
     // if (vertexBuffer) { // Removed
     //    [vertexBuffer release];
     //    vertexBuffer = nil;
     // }
    if (textureCache) {
        CVMetalTextureCacheFlush(textureCache, 0); // Очищаем кэш перед освобождением
        CFRelease(textureCache);
        textureCache = nullptr;
    }
    if (commandQueue) {
        [commandQueue release]; // Release the queue we created
        commandQueue = nil;
    }
    if (device) {
         // Device obtained from SDL is likely autoreleased, no explicit release needed.
         device = nil;
    }

    initialized = false;
    std::cout << "[MetalRenderer] Cleanup complete." << std::endl;
}

// --- Приватные методы --- 

bool MetalRenderer::setupRenderPipeline() {
    std::cout << "[MetalRenderer] Setting up Metal pipeline state..." << std::endl;
    if (!device) return false;

    // --- Шейдеры --- 
    const char* vertexShaderSource = R"(
        using namespace metal;

        struct VertexOutput {
            float4 position [[position]];
            float2 texCoord;
        };

        struct VertexInput {
            float2 position [[attribute(0)]];
            float2 texCoord [[attribute(1)]];
        };

        vertex VertexOutput vertex_main(VertexInput input [[stage_in]]) {
            VertexOutput out;
            out.position = float4(input.position, 0.0, 1.0);
            out.texCoord = input.texCoord;
            return out;
        }
    )";

    const char* fragmentShaderSource = R"(
        using namespace metal;

        struct VertexOutput {
            float4 position [[position]];
            float2 texCoord;
        };

        // BT.709 Limited Range YCbCr to RGB Conversion Matrix (Column-Major for Metal)
        constant float3x3 colorConversionMatrix = float3x3(
            float3(1.164383,  1.164383, 1.164383), // Column 0: Y' multipliers for R, G, B
            float3(0.0,      -0.391762, 2.017232), // Column 1: Cb' multipliers for R, G, B
            float3(1.596027, -0.812968, 0.0)       // Column 2: Cr' multipliers for R, G, B
        );

        // Offset to center limited range (Y:16-235 -> -16..219, Cb/Cr:16-240 -> -128..112) before matrix multiplication
        constant float3 colorOffset = float3(-16.0/255.0, -128.0/255.0, -128.0/255.0);

        fragment float4 fragment_main(VertexOutput input [[stage_in]],
                                      texture2d<float, access::sample> yTexture [[texture(0)]],
                                      texture2d<float, access::sample> uvTexture [[texture(1)]])
        {
            constexpr sampler s(coord::normalized, filter::linear);

            float y = yTexture.sample(s, input.texCoord).r;
            float2 uv = uvTexture.sample(s, input.texCoord).rg;

            // Apply offset first, then multiply by the conversion matrix
            float3 ycbcr = float3(y, uv.x, uv.y) + colorOffset;
            float3 rgb = colorConversionMatrix * ycbcr; // Use correct matrix name

            // Clamp the result to [0, 1] range to avoid potential issues with over/undershoot
            float3 clampedRGB = clamp(rgb, 0.0, 1.0);

            return float4(clampedRGB, 1.0);
        }
    )";

    NSError* error = nil;

    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:vertexShaderSource]
                                                  options:nil
                                                    error:&error];
    if (!library || error) {
        std::cerr << "[MetalRenderer] Failed to create library from vertex shader: "
                  << (error ? [[error localizedDescription] UTF8String] : "Unknown error") << std::endl;
        if(error) [error release];
        return false;
    }

    id<MTLLibrary> fragmentLibrary = [device newLibraryWithSource:[NSString stringWithUTF8String:fragmentShaderSource]
                                                           options:nil
                                                             error:&error];
     if (!fragmentLibrary || error) {
        std::cerr << "[MetalRenderer] Failed to create library from fragment shader: "
                  << (error ? [[error localizedDescription] UTF8String] : "Unknown error") << std::endl;
        [library release];
        if(error) [error release];
        return false;
    }

    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragmentFunction = [fragmentLibrary newFunctionWithName:@"fragment_main"];
    [library release];
    [fragmentLibrary release];

    if (!vertexFunction || !fragmentFunction) {
        std::cerr << "[MetalRenderer] Failed to get shader functions." << std::endl;
        if(vertexFunction) [vertexFunction release];
        if(fragmentFunction) [fragmentFunction release];
        return false;
    }

    // --- Vertex Descriptor --- 
    MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];

    // Position attribute
    vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2; // vec2
    vertexDescriptor.attributes[0].offset = 0;                     // Starts at the beginning of the struct
    vertexDescriptor.attributes[0].bufferIndex = 0;                // Use the buffer bound at index 0

    // Texture Coordinate attribute
    vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2; // vec2
    vertexDescriptor.attributes[1].offset = sizeof(float) * 2;     // Offset by the size of the position (2 floats)
    vertexDescriptor.attributes[1].bufferIndex = 0;                // Also from the buffer bound at index 0

    // Vertex Buffer Layout
    vertexDescriptor.layouts[0].stride = sizeof(float) * 4; // Size of one complete vertex (pos + texCoord)
    vertexDescriptor.layouts[0].stepRate = 1;
    vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    // --- Render Pipeline Descriptor --- 
    MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.label = @"VideoRenderPipeline";
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.vertexDescriptor = vertexDescriptor; // Assign the vertex descriptor
    pipelineDescriptor.colorAttachments[0].pixelFormat = layer.pixelFormat; // Use the layer's pixel format

    renderPipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error]; // Assign to renderPipelineState

    [pipelineDescriptor release];
    [vertexDescriptor release]; // Release the vertex descriptor
    [vertexFunction release];
    [fragmentFunction release];

    if (!renderPipelineState || error) {
        std::cerr << "[MetalRenderer] Failed to create render pipeline state: "
                   << (error ? [[error localizedDescription] UTF8String] : "Unknown error") << std::endl;
        if(error) [error release];
        return false;
    }

    std::cout << "[MetalRenderer] Pipeline state created." << std::endl;
    return true;
}

// bool MetalRenderer::createQuadVertexBuffer() { // Removed entire function
//      std::cout << "[MetalRenderer] Creating quad vertex buffer..." << std::endl;
//      if (!device) return false;
// 
//     // Vertex data: {position.x, position.y, texCoord.u, texCoord.v}
//     // 4 vertices for a TriangleStrip covering the screen/quad
//     static const float vertexData[] = {
//         // Position      TexCoord
//         -1.0f, -1.0f,   0.0f, 1.0f, // Bottom-left
//          1.0f, -1.0f,   1.0f, 1.0f, // Bottom-right
//         -1.0f,  1.0f,   0.0f, 0.0f, // Top-left
//          1.0f,  1.0f,   1.0f, 0.0f  // Top-right
//     };
// 
//     vertexBuffer = [device newBufferWithBytes:vertexData
//                                       length:sizeof(vertexData)
//                                      options:MTLResourceStorageModeShared];
// 
//     if (!vertexBuffer) {
//          std::cerr << "[MetalRenderer] Failed to create vertex buffer." << std::endl;
//          return false;
//     }
//     vertexBuffer.label = @"QuadVertices";
// 
//     std::cout << "[MetalRenderer] Vertex buffer created." << std::endl;
//     return true;
// }

// --- Рендеринг --- 
bool MetalRenderer::render(CVPixelBufferRef pixelBuffer, SDL_Renderer* sdlRenderer) {
#ifdef __APPLE__
    if (!initialized || !pixelBuffer || !sdlRenderer) {
        std::cerr << "[MetalRenderer::render] Early exit: Initialized=" << initialized 
                  << ", pixelBuffer=" << (pixelBuffer ? "valid" : "null") 
                  << ", sdlRenderer=" << (sdlRenderer ? "valid" : "null") << std::endl;
        return false;
    }
    // std::cout << "[MetalRenderer::render] Called with valid pixelBuffer." << std::endl; // Optional: Log every call

    // Get the Metal command encoder from SDL
    // Cast the void* returned by SDL to the correct Objective-C type
    id<MTLRenderCommandEncoder> renderEncoder = (__bridge id<MTLRenderCommandEncoder>)SDL_RenderGetMetalCommandEncoder(sdlRenderer);

    if (!renderEncoder) {
        std::cerr << "[MetalRenderer] Error: Could not get MTLRenderCommandEncoder from SDL_Renderer." << std::endl;
        // This might happen if SDL isn't actively rendering with Metal at this moment.
        // Consider managing command buffers outside SDL's direct loop if needed.
        return false;
    }

    // Create textures from the CVPixelBufferRef using the cache
    bool texturesCreated = createMetalTexturesFromBuffer(pixelBuffer); // Store result
    if (!texturesCreated) {
        std::cerr << "[MetalRenderer::render] Failed to create Metal textures from pixel buffer. Aborting render." << std::endl;
        // Don't end encoding here, let SDL handle it maybe? Or should we?
        // [renderEncoder endEncoding]; // Maybe needed? Test this.
        return false;
    }
    // std::cout << "[MetalRenderer::render] Textures created successfully." << std::endl; // Optional log

    // Now we have yTexture and cbcrTexture populated (or plane0Texture for non-planar)

    // --- Calculate Aspect Ratio Corrected Vertices ---
    size_t videoWidth = CVPixelBufferGetWidth(pixelBuffer);
    size_t videoHeight = CVPixelBufferGetHeight(pixelBuffer);
    CGSize drawableSize = layer.drawableSize;

    if (videoWidth == 0 || videoHeight == 0 || drawableSize.width == 0 || drawableSize.height == 0) {
        std::cerr << "[MetalRenderer] Error: Invalid dimensions for aspect ratio calculation." << std::endl;
        return false; // Avoid division by zero
    }

    float videoAspect = (float)videoWidth / (float)videoHeight;
    float drawableAspect = (float)drawableSize.width / (float)drawableSize.height;

    float scaleX = 1.0f;
    float scaleY = 1.0f;

    if (videoAspect > drawableAspect) { // Video wider than drawable (letterbox)
        scaleY = drawableAspect / videoAspect;
    } else { // Video narrower than drawable (pillarbox)
        scaleX = videoAspect / drawableAspect;
    }

    // Adjusted vertex data: {pos.x, pos.y, texCoord.u, texCoord.v}
    float adjustedVertexData[] = {
        // Position (scaled)       TexCoord
        -1.0f * scaleX, -1.0f * scaleY,   0.0f, 1.0f, // Bottom-left
         1.0f * scaleX, -1.0f * scaleY,   1.0f, 1.0f, // Bottom-right
        -1.0f * scaleX,  1.0f * scaleY,   0.0f, 0.0f, // Top-left
         1.0f * scaleX,  1.0f * scaleY,   1.0f, 0.0f  // Top-right
    };

    // Set pipeline state
    [renderEncoder setRenderPipelineState:renderPipelineState]; // Use renderPipelineState

    // Set vertex data dynamically for this frame
    [renderEncoder setVertexBytes:adjustedVertexData length:sizeof(adjustedVertexData) atIndex:0];
    // [renderEncoder setVertexBuffer:vertexBuffer offset:0 atIndex:0]; // Removed: using setVertexBytes instead

    // Set textures
    if (CVPixelBufferGetPlaneCount(pixelBuffer) == 2) { // NV12 or P010 etc.
         if (yTexture) [renderEncoder setFragmentTexture:yTexture atIndex:0];
         if (cbcrTexture) [renderEncoder setFragmentTexture:cbcrTexture atIndex:1];
    } else if (CVPixelBufferGetPlaneCount(pixelBuffer) == 1) { // BGRA etc.
         if (plane0Texture) [renderEncoder setFragmentTexture:plane0Texture atIndex:0]; // Assuming shader uses index 0
    }
    // Add more cases if supporting other planar formats like YUV420p (3 planes)

    // Set sampler state (optional, if using samplers in shader)
    // [renderEncoder setFragmentSamplerState:samplerState atIndex:0];

    // Draw the quad
    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    // Textures (yTexture, cbcrTexture, plane0Texture) are autoreleased (__bridge id<MTLTexture>)
    // managed by the CVMetalTextureCacheRef, no need to release them manually here.
    // The CVMetalTextureRef itself (cvTextureY, cvTextureCbCr) was released in createMetalTexturesFromBuffer.

    // Note: We are *not* calling [renderEncoder endEncoding] here.
    // SDL manages the render command encoder lifetime when using SDL_RenderGetMetalCommandEncoder.
    // Ending it here would likely interfere with SDL's rendering process.

    return true;
#else
    return false; // Metal not supported
#endif // __APPLE__
}

bool MetalRenderer::createMetalTexturesFromBuffer(CVPixelBufferRef pixelBuffer) {
#ifdef __APPLE__
    if (!initialized || !pixelBuffer || !textureCache) {
        std::cerr << "[MetalRenderer::createMetalTextures] Early exit: Initialized=" << initialized 
                  << ", pixelBuffer=" << (pixelBuffer ? "valid" : "null") 
                  << ", textureCache=" << (textureCache ? "valid" : "null") << std::endl;
        return false;
    }

    // --- Flush the texture cache before creating new textures --- 
    // This might help with stale data issues during rapid updates (like seeking)
    CVMetalTextureCacheFlush(textureCache, 0); 

    // Release previous textures if they exist (the cache should handle reuse, but let's be explicit)
    // yTexture = nil;       // ARC will handle release
    // cbcrTexture = nil;    // ARC will handle release
    // plane0Texture = nil; // ARC will handle release

    size_t width = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);
    OSType format = CVPixelBufferGetPixelFormatType(pixelBuffer);
    size_t planeCount = CVPixelBufferGetPlaneCount(pixelBuffer);

    // Log input buffer details
    // std::cout << "[MetalRenderer::createMetalTextures] Input CVPixelBuffer: Format=" << format << ", Planes=" << planeCount << ", W=" << width << ", H=" << height << std::endl;

    CVReturn err;

    if (planeCount == 2 && (format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
        // NV12 format (Y plane and CbCr plane)
        MTLPixelFormat textureFormatY = MTLPixelFormatR8Unorm;
        MTLPixelFormat textureFormatCbCr = MTLPixelFormatRG8Unorm;

        // Temporary CoreVideo texture references
        CVMetalTextureRef cvTextureY = nullptr;
        CVMetalTextureRef cvTextureCbCr = nullptr;

        // Create Y texture
        size_t yWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 0);
        size_t yHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0);
        err = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, textureCache, pixelBuffer, nil,
                                                        textureFormatY, yWidth, yHeight, 0, &cvTextureY);
        if (err != kCVReturnSuccess || cvTextureY == nullptr) {
            std::cerr << "[MetalRenderer::createMetalTextures] Error creating Y texture from cache: CVReturn=" << err << ", cvTextureY=" << (cvTextureY ? "valid" : "null") << std::endl;
            if(cvTextureY) CFRelease(cvTextureY); // Release if created before failure
            return false;
        }

        // Create CbCr texture
        size_t cbcrWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 1);
        size_t cbcrHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 1);
        err = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, textureCache, pixelBuffer, nil,
                                                        textureFormatCbCr, cbcrWidth, cbcrHeight, 1, &cvTextureCbCr);
        if (err != kCVReturnSuccess || cvTextureCbCr == nullptr) {
            std::cerr << "[MetalRenderer::createMetalTextures] Error creating CbCr texture from cache: CVReturn=" << err << ", cvTextureCbCr=" << (cvTextureCbCr ? "valid" : "null") << std::endl;
            CFRelease(cvTextureY); // Release Y texture
             if(cvTextureCbCr) CFRelease(cvTextureCbCr); // Release if created before failure
            return false;
        }

        // Get the underlying Metal textures
        yTexture = CVMetalTextureGetTexture(cvTextureY);
        cbcrTexture = CVMetalTextureGetTexture(cvTextureCbCr);
        // std::cout << "[MetalRenderer::createMetalTextures] Got underlying textures: yTexture=" << (yTexture ? "valid" : "null") << ", cbcrTexture=" << (cbcrTexture ? "valid" : "null") << std::endl;

        // Release the CoreVideo texture references (Metal textures are now managed by the cache/ARC)
        CFRelease(cvTextureY);
        CFRelease(cvTextureCbCr);

        plane0Texture = nil; // Ensure single-plane texture is nil

        if (!yTexture || !cbcrTexture) {
             std::cerr << "[MetalRenderer::createMetalTextures] Failed to get underlying MTLTexture from CVMetalTextureRef." << std::endl;
             yTexture = nil; cbcrTexture = nil; // Reset on failure
             return false;
        }
         //std::cout << "[MetalRenderer] Created NV12 Textures: Y(" << yWidth << "x" << yHeight << "), CbCr(" << cbcrWidth << "x" << cbcrHeight << ")" << std::endl;

    } else if (planeCount == 1 && format == kCVPixelFormatType_32BGRA) {
         // BGRA format (single plane)
         MTLPixelFormat textureFormat = MTLPixelFormatBGRA8Unorm; // Or BGRA8Unorm_sRGB if color space requires

         CVMetalTextureRef cvTexture = nullptr;

         err = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, textureCache, pixelBuffer, nil,
                                                         textureFormat, width, height, 0, &cvTexture);
         if (err != kCVReturnSuccess || cvTexture == nullptr) {
             std::cerr << "[MetalRenderer::createMetalTextures] Error creating BGRA texture from cache: CVReturn=" << err << ", cvTexture=" << (cvTexture ? "valid" : "null") << std::endl;
              if(cvTexture) CFRelease(cvTexture);
             return false;
         }

         plane0Texture = CVMetalTextureGetTexture(cvTexture);
         CFRelease(cvTexture);

         yTexture = nil; // Ensure multi-plane textures are nil
         cbcrTexture = nil;

         if(!plane0Texture){
            std::cerr << "[MetalRenderer::createMetalTextures] Failed to get underlying MTLTexture from CVMetalTextureRef for BGRA." << std::endl;
            return false;
         }
         //std::cout << "[MetalRenderer] Created BGRA Texture: " << width << "x" << height << std::endl;

    } else {
        std::cerr << "[MetalRenderer::createMetalTextures] Error: Unsupported CVPixelBuffer format: " << format << " with plane count: " << planeCount << std::endl;
        return false;
    }

    // Flush the cache (might not be strictly necessary, but can help ensure texture data is ready)
    // CVMetalTextureCacheFlush(textureCache, 0); // Use with caution, might have performance implications

    return true;
#else
    return false; // Metal not supported
#endif // __APPLE__
}

#endif // __APPLE__