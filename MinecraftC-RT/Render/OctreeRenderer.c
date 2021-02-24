#include <SDL2/SDL.h>
#include <OpenGL.h>
#include "OctreeRenderer.h"
#include "../Level/Level.h"
#include "../Utilities/Log.h"
#include "../Utilities/Memory.h"

struct OctreeRenderer OctreeRenderer = { 0 };

void OctreeRendererInitialize(int width, int height)
{
	OctreeRenderer.Width = width;
	OctreeRenderer.Height = height;
	OctreeRenderer.TextureData = MemoryAllocate(width * height * sizeof(unsigned int));
	glGenTextures(1, &OctreeRenderer.TextureID);
	glBindTexture(GL_TEXTURE_2D, OctreeRenderer.TextureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, OctreeRenderer.TextureData);
	
	cl_platform_id platform;
	if (clGetPlatformIDs(1, &platform, NULL) < 0) { LogFatal("Couldn't find a suitable platform for OpenCL\n"); }
	if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &OctreeRenderer.Device, NULL) == CL_DEVICE_NOT_FOUND) { LogFatal("Sorry bitch you don't have a GPU\n"); }
	
	int error;
	OctreeRenderer.Context = clCreateContext(NULL, 1, &OctreeRenderer.Device, NULL, NULL, &error);
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
		char log[1024];
		clGetProgramBuildInfo(OctreeRenderer.Shader, OctreeRenderer.Device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
		LogFatal("Failed to compile shader program: %s\n", log);
	}
	
	OctreeRenderer.Queue = clCreateCommandQueue(OctreeRenderer.Context, OctreeRenderer.Device, 0, &error);
	if (error < 0) { LogFatal("Failed to create command queue: %i\n", error); }
	OctreeRenderer.Kernel = clCreateKernel(OctreeRenderer.Shader, "trace", &error);
	if (error < 0) { LogFatal("Failed to create kernel: %i\n", error); }
	
	OctreeRenderer.TextureBuffer = clCreateBuffer(OctreeRenderer.Context, CL_MEM_WRITE_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(unsigned int) * width * height, OctreeRenderer.TextureData, &error);
	if (error < 0) { LogFatal("Failed to create texture buffer: %i\n", error); }
	error = clSetKernelArg(OctreeRenderer.Kernel, 3, sizeof(int), &OctreeRenderer.Width);
	error |= clSetKernelArg(OctreeRenderer.Kernel, 4, sizeof(int), &OctreeRenderer.Height);
	error |= clSetKernelArg(OctreeRenderer.Kernel, 5, sizeof(cl_mem), &OctreeRenderer.TextureBuffer);
	if (error < 0) { LogFatal("Failed to set kernel arguments: %i\n", error); }
}

void OctreeRendererSetOctree(Octree tree)
{
	OctreeRenderer.Octree = tree;
	
	int error;
	OctreeRenderer.OctreeBuffer = clCreateBuffer(OctreeRenderer.Context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, tree->MaskCount, tree->Masks, &error);
	if (error < 0) { LogFatal("Failed to create octree buffer: %i\n", error); }
	OctreeRenderer.BlockBuffer = clCreateBuffer(OctreeRenderer.Context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, tree->Level->Width * tree->Level->Height * tree->Level->Depth, tree->Level->Blocks, &error);
	if (error < 0) { LogFatal("Failed to create block buffer: %i\n", error); }
	
	error = clSetKernelArg(OctreeRenderer.Kernel, 0, sizeof(unsigned int), &tree->Depth);
	error |= clSetKernelArg(OctreeRenderer.Kernel, 1, sizeof(cl_mem), &OctreeRenderer.OctreeBuffer);
	error |= clSetKernelArg(OctreeRenderer.Kernel, 2, sizeof(cl_mem), &OctreeRenderer.BlockBuffer);
	if (error < 0) { LogFatal("Failed to set kernel arguments: %i\n", error); }
}

void OctreeRendererDeinitialize()
{
	clReleaseMemObject(OctreeRenderer.TextureBuffer);
	clReleaseMemObject(OctreeRenderer.OctreeBuffer);
	clReleaseMemObject(OctreeRenderer.BlockBuffer);
	clReleaseKernel(OctreeRenderer.Kernel);
	clReleaseCommandQueue(OctreeRenderer.Queue);
	clReleaseProgram(OctreeRenderer.Shader);
	clReleaseContext(OctreeRenderer.Context);
	clReleaseDevice(OctreeRenderer.Device);
	glDeleteTextures(1, &OctreeRenderer.TextureID);
	MemoryFree(OctreeRenderer.TextureData);
}