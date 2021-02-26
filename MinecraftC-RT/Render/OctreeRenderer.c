#include <SDL2/SDL.h>
#include <OpenGL.h>
#include "OctreeRenderer.h"
#include "../Level/Level.h"
#include "../Player/Player.h"
#include "../Utilities/Log.h"
#include "../Utilities/Memory.h"

struct OctreeRenderer OctreeRenderer = { 0 };

void OctreeRendererInitialize(TextureManager textures, int width, int height)
{
	OctreeRenderer.Width = width;
	OctreeRenderer.Height = height;
	OctreeRenderer.TextureManager = textures;
	glGenTextures(1, &OctreeRenderer.TextureID);
	glBindTexture(GL_TEXTURE_2D, OctreeRenderer.TextureID);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	
	cl_platform_id platform;
	unsigned int platformCount = 0;
	clGetPlatformIDs(0, NULL, &platformCount);
	cl_platform_id * platforms = MemoryAllocate(platformCount * sizeof(cl_platform_id));
	if (clGetPlatformIDs(platformCount, platforms, NULL) < 0) { LogFatal("Couldn't find a suitable platform for OpenCL\n"); }
	for (int i = 0; i < platformCount; i++)
	{
		char name[1024];
		clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, 1024, name, NULL);
		LogDebug("%s\n", name);
	}
	platform = platforms[0];
	MemoryFree(platforms);

	unsigned int deviceCount = 0;
	clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &deviceCount);
	cl_device_id * devices = MemoryAllocate(deviceCount * sizeof(cl_device_id));
	if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, deviceCount, devices, NULL) == CL_DEVICE_NOT_FOUND) { LogFatal("No supported GPU found\n"); }
	char name[128];
	clGetDeviceInfo(devices[0], CL_DEVICE_VENDOR, 128, name, NULL);
	LogDebug("%s\n", name);
	OctreeRenderer.Device = devices[0];
	MemoryFree(devices);
	
	cl_context_properties properties[] =
	{
#ifdef __APPLE__
		CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE,
		(cl_context_properties)CGLGetShareGroup(CGLGetCurrentContext()),
#elif defined(_WIN32)
		CL_GL_CONTEXT_KHR, (cl_context_properties)wglGetCurrentContext(),
		CL_WGL_HDC_KHR, (cl_context_properties)wglGetCurrentDC(),
		CL_CONTEXT_PLATFORM, (cl_context_properties)platform,
#elif defined(__linux__)
		CL_GL_CONTEXT_KHR, (cl_context_properties)glXGetCurrentContext(),
		CL_GLX_DISPLAY_KHR, (cl_context_properties)glXGetCurrentDisplay(),
		CL_CONTEXT_PLATFORM, (cl_context_properties)platform,
#endif
		0,
	};

	int error;
	OctreeRenderer.Context = clCreateContext(properties, 1, &OctreeRenderer.Device, NULL, NULL, &error);
	if (error < 0) { LogFatal("Failed to create context: %i\n", error); }
	
	SDL_RWops * shaderFile = SDL_RWFromFile("Shaders/Raytracer.cl", "r");
	if (shaderFile == NULL) { LogFatal("Failed to open Raytracer.cl: %s\n", SDL_GetError()); }
	size_t fileSize = (int)SDL_RWseek(shaderFile, 0, RW_SEEK_END);
	SDL_RWseek(shaderFile, 0, RW_SEEK_SET);
	char * shaderText = MemoryAllocate(fileSize + 1);
	SDL_RWread(shaderFile, shaderText, fileSize, 1);
	SDL_RWclose(shaderFile);
	shaderText[fileSize] = '\0';
	OctreeRenderer.Shader = clCreateProgramWithSource(OctreeRenderer.Context, 1, (const char **)&shaderText, &fileSize, &error);
	if (error < 0) { LogFatal("Failed to create shader program: %i\n", error); }
	MemoryFree(shaderText);
	
	error = clBuildProgram(OctreeRenderer.Shader, 0, NULL, NULL, NULL, NULL);
	if (error < 0)
	{
		size_t logSize;
		clGetProgramBuildInfo(OctreeRenderer.Shader, OctreeRenderer.Device, CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);
		char * log = MemoryAllocate(logSize);
		clGetProgramBuildInfo(OctreeRenderer.Shader, OctreeRenderer.Device, CL_PROGRAM_BUILD_LOG, logSize, log, NULL);
		LogFatal("Failed to compile shader program: %s\n", log);
		MemoryFree(log);
	}
	
	OctreeRenderer.Queue = clCreateCommandQueue(OctreeRenderer.Context, OctreeRenderer.Device, 0, &error);
	if (error < 0) { LogFatal("Failed to create command queue: %i\n", error); }
	OctreeRenderer.Kernel = clCreateKernel(OctreeRenderer.Shader, "trace", &error);
	if (error < 0) { LogFatal("Failed to create kernel: %i\n", error); }
	
	OctreeRenderer.OutputTexture = clCreateFromGLTexture(OctreeRenderer.Context, CL_MEM_WRITE_ONLY, GL_TEXTURE_2D, 0, OctreeRenderer.TextureID, &error);
	if (error < 0) { LogFatal("Failed to create texture buffer: %i\n", error); }
	error = clSetKernelArg(OctreeRenderer.Kernel, 3, sizeof(cl_mem), &OctreeRenderer.OutputTexture);
	if (error < 0) { LogFatal("Failed to set kernel arguments: %i\n", error); }
	
	OctreeRenderer.TerrainTexture = clCreateFromGLTexture(OctreeRenderer.Context, CL_MEM_READ_ONLY, GL_TEXTURE_2D, 0, TextureManagerLoad(textures, "Terrain.png"), &error);
	if (error < 0) { LogFatal("Failed to create texture buffer: %i\n", error); }
	error = clSetKernelArg(OctreeRenderer.Kernel, 5, sizeof(cl_mem), &OctreeRenderer.TerrainTexture);
	if (error < 0) { LogFatal("Failed to set kernel arguments: %i\n", error); }
}

void OctreeRendererResize(int width, int height)
{
	OctreeRendererDeinitialize();
	OctreeRendererInitialize(OctreeRenderer.TextureManager, width, height);
	OctreeRendererSetOctree(OctreeRenderer.Octree);
}

void OctreeRendererSetOctree(Octree tree)
{
	OctreeRenderer.Octree = tree;
	
	int error;
	OctreeRenderer.OctreeBuffer = clCreateBuffer(OctreeRenderer.Context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, tree->MaskCount, tree->Masks, &error);
	if (error < 0) { LogFatal("Failed to create octree buffer: %i\n", error); }
	OctreeRenderer.BlockBuffer = clCreateBuffer(OctreeRenderer.Context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, tree->Level->Width * tree->Level->Height * tree->Level->Depth, tree->Level->Blocks, &error);
	if (error < 0) { LogFatal("Failed to create block buffer: %i\n", error); }
	
	error = clSetKernelArg(OctreeRenderer.Kernel, 0, sizeof(unsigned int), &tree->Depth);
	error |= clSetKernelArg(OctreeRenderer.Kernel, 1, sizeof(cl_mem), &OctreeRenderer.OctreeBuffer);
	error |= clSetKernelArg(OctreeRenderer.Kernel, 2, sizeof(cl_mem), &OctreeRenderer.BlockBuffer);
	if (error < 0) { LogFatal("Failed to set kernel arguments: %i\n", error); }
}

void OctreeRendererEnqueue()
{
	glFinish();
	Player player = OctreeRenderer.Octree->Level->Player;
	Matrix4x4 camera = Matrix4x4Multiply(Matrix4x4FromTranslate(player->Position), Matrix4x4FromEulerAngles((float3){ 180.0 - player->Rotation.y, player->Rotation.x, 0.0 } * rad));
	int error = clSetKernelArg(OctreeRenderer.Kernel, 4, sizeof(Matrix4x4), &camera);
	if (error < 0) { LogFatal("Failed to set kernel argument: %i\n", error); }
	error = clEnqueueAcquireGLObjects(OctreeRenderer.Queue, 2, (cl_mem[]){ OctreeRenderer.OutputTexture, OctreeRenderer.TerrainTexture }, 0, NULL, NULL);
	if (error < 0) { LogFatal("Failed to aquire gl texture: %i\n"); }
	int groupSize = 50;
	int w = OctreeRenderer.Width, h = OctreeRenderer.Height;
	error = clEnqueueNDRangeKernel(OctreeRenderer.Queue, OctreeRenderer.Kernel, 2, NULL, (size_t[]){ w - w % groupSize, h - h % groupSize }, (size_t[]){ groupSize, 1 }, 0, NULL, NULL);
	if (error < 0) { LogFatal("Failed to enqueue octree renderer: %i\n"); }
	error = clEnqueueReleaseGLObjects(OctreeRenderer.Queue, 2, (cl_mem[]){ OctreeRenderer.OutputTexture, OctreeRenderer.TerrainTexture }, 0, NULL, NULL);
	if (error < 0) { LogFatal("Failed to release gl texture: %i\n"); }
	clFinish(OctreeRenderer.Queue);
}

void OctreeRendererDeinitialize()
{
	clReleaseMemObject(OctreeRenderer.OutputTexture);
	clReleaseMemObject(OctreeRenderer.OctreeBuffer);
	clReleaseMemObject(OctreeRenderer.BlockBuffer);
	clReleaseMemObject(OctreeRenderer.TerrainTexture);
	clReleaseKernel(OctreeRenderer.Kernel);
	clReleaseCommandQueue(OctreeRenderer.Queue);
	clReleaseProgram(OctreeRenderer.Shader);
	clReleaseContext(OctreeRenderer.Context);
	clReleaseDevice(OctreeRenderer.Device);
	glDeleteTextures(1, &OctreeRenderer.TextureID);
}
