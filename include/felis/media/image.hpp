#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <filesystem>
#include <optional>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <GL/gl.h>

namespace felis::media
{

	struct texture
	{
		GLuint id = 0;
		int width = 0;
		int height = 0;
	};

	std::optional<texture> load_texture(const std::filesystem::path& path);
	std::optional<texture> load_png_texture(const std::filesystem::path& path);
	std::optional<texture> load_jpeg_texture(const std::filesystem::path& path);

}
