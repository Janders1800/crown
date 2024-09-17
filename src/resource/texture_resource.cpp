/*
 * Copyright (c) 2012-2024 Daniele Bartolini et al.
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "core/filesystem/reader_writer.inl"
#include "core/json/json_object.inl"
#include "core/json/sjson.h"
#include "core/memory/temp_allocator.inl"
#include "core/process.h"
#include "core/strings/dynamic_string.inl"
#include "core/strings/string_id.inl"
#include "core/strings/string_stream.inl"
#include "resource/compile_options.inl"
#include "resource/resource_manager.h"
#include "resource/texture_resource.h"

namespace crown
{
namespace texture_resource_internal
{
	void *load(File &file, Allocator &a)
	{
		BinaryReader br(file);

		u32 version;
		br.read(version);
		CE_ASSERT(version == RESOURCE_HEADER(RESOURCE_VERSION_TEXTURE), "Wrong version");

		u32 size;
		br.read(size);

		TextureResource *tr = (TextureResource *)a.allocate(sizeof(TextureResource) + size);

		void *data = &tr[1];
		br.read(data, size);

		tr->mem        = bgfx::makeRef(data, size);
		tr->handle.idx = BGFX_INVALID_HANDLE;

		return tr;
	}

	void online(StringId64 id, ResourceManager &rm)
	{
		TextureResource *tr = (TextureResource *)rm.get(RESOURCE_TYPE_TEXTURE, id);
		tr->handle = bgfx::createTexture(tr->mem);
	}

	void offline(StringId64 id, ResourceManager &rm)
	{
		TextureResource *tr = (TextureResource *)rm.get(RESOURCE_TYPE_TEXTURE, id);
		bgfx::destroy(tr->handle);
	}

	void unload(Allocator &a, void *resource)
	{
		a.deallocate(resource);
	}

} // namespace texture_resource_internal

#if CROWN_CAN_COMPILE
namespace texture_resource_internal
{
	static const char *texturec_paths[] =
	{
		EXE_PATH("texturec"),
#if CROWN_DEBUG
		EXE_PATH("texturec-debug")
#elif CROWN_DEVELOPMENT
		EXE_PATH("texturec-development")
#else
		EXE_PATH("texturec-release")
#endif
	};

	struct TextureFormat
	{
		enum Enum
		{
			BC1,
			BC2,
			BC3,
			BC4,
			BC5,
			PTC14,
			RGB8,
			RGBA8,

			COUNT
		};
	};

	struct TextureFormatInfo
	{
		const char *name;
		TextureFormat::Enum type;
	};

	static const TextureFormatInfo s_texture_formats[] =
	{
		{ "BC1",   TextureFormat::BC1   },
		{ "BC2",   TextureFormat::BC2   },
		{ "BC3",   TextureFormat::BC3   },
		{ "BC4",   TextureFormat::BC4   },
		{ "BC5",   TextureFormat::BC5   },
		{ "PTC14", TextureFormat::PTC14 },
		{ "RGB8",  TextureFormat::RGB8  },
		{ "RGBA8", TextureFormat::RGBA8 },
	};
	CE_STATIC_ASSERT(countof(s_texture_formats) == TextureFormat::COUNT);

	static TextureFormat::Enum texture_format_to_enum(const char *format)
	{
		for (u32 i = 0; i < countof(s_texture_formats); ++i) {
			if (strcmp(format, s_texture_formats[i].name) == 0)
				return s_texture_formats[i].type;
		}

		return TextureFormat::COUNT;
	}

	struct OutputSettings
	{
		TextureFormat::Enum format; ///< Output format.
		bool generate_mips;         ///< Whether to generate mip-maps.
		u32 mip_skip_smallest;      ///< Number of (smallest) mip steps to skip.
		bool normal_map;            ///< Whether to skip gamma correction.

		OutputSettings()
			: format(TextureFormat::RGBA8)
			, generate_mips(true)
			, mip_skip_smallest(0u)
			, normal_map(false)
		{
		}
	};

	s32 parse_output(OutputSettings &os, JsonObject &output, CompileOptions &opts)
	{
		const char *platform = opts.platform_name();

		if (json_object::has(output, platform)) {
			TempAllocator1024 ta;
			JsonObject obj(ta);
			sjson::parse_object(obj, output[platform]);

			DynamicString format(ta);
			if (json_object::has(obj, "format")) {
				sjson::parse_string(format, obj["format"]);
				os.format = texture_format_to_enum(format.c_str());
				DATA_COMPILER_ASSERT(os.format != TextureFormat::COUNT
					, opts
					, "Unknown texture format: '%s'"
					, format.c_str()
					);
			}
			if (json_object::has(obj, "generate_mips"))
				os.generate_mips     = sjson::parse_bool(obj["generate_mips"]);
			if (json_object::has(obj, "mip_skip_smallest"))
				os.mip_skip_smallest = sjson::parse_int (obj["mip_skip_smallest"]);
			if (json_object::has(obj, "normal_map"))
				os.normal_map        = sjson::parse_bool(obj["normal_map"]);
		}

		return 0;
	}

	s32 compile(CompileOptions &opts)
	{
		Buffer buf = opts.read();

		TempAllocator4096 ta;
		JsonObject obj(ta);
		sjson::parse(obj, buf);

		DynamicString name(ta);
		sjson::parse_string(name, obj["source"]);
		DATA_COMPILER_ASSERT_FILE_EXISTS(name.c_str(), opts);
		opts.fake_read(name.c_str());

		OutputSettings os;

		JsonObject output(ta);
		if (json_object::has(obj, "output")) {
			sjson::parse_object(output, obj["output"]);
			s32 err = parse_output(os, output, opts);
			DATA_COMPILER_ENSURE(err == 0, opts);
		} else {
			os.generate_mips = sjson::parse_bool(obj["generate_mips"]);
			os.normal_map    = sjson::parse_bool(obj["normal_map"]);
		}

		DynamicString tex_src(ta);
		DynamicString tex_out(ta);
		opts.absolute_path(tex_src, name.c_str());
		opts.temporary_path(tex_out, "ktx");

		const char *texturec = opts.exe_path(texturec_paths, countof(texturec_paths));
		DATA_COMPILER_ASSERT(texturec != NULL
			, opts
			, "texturec not found"
			);

		char mipskip[16];
		stbsp_snprintf(mipskip, sizeof(mipskip), "--mipskip %u", os.mip_skip_smallest);

		const char *argv[] =
		{
			texturec,
			"-f",
			tex_src.c_str(),
			"-o",
			tex_out.c_str(),
			"-t",
			s_texture_formats[os.format].name,
			(os.normal_map ? "-n" : ""),
			(os.generate_mips ? "-m" : ""),
			(os.mip_skip_smallest > 0 ? "--mipskip" : ""),
			(os.mip_skip_smallest > 0 ? mipskip : ""),
			NULL
		};
		Process pr;
		s32 sc = pr.spawn(argv, CROWN_PROCESS_STDOUT_PIPE | CROWN_PROCESS_STDERR_MERGE);
		DATA_COMPILER_ASSERT(sc == 0
			, opts
			, "Failed to spawn `%s`"
			, argv[0]
			);
		StringStream texturec_output(ta);
		opts.read_output(texturec_output, pr);
		s32 ec = pr.wait();
		DATA_COMPILER_ASSERT(ec == 0
			, opts
			, "Failed to compile texture:\n%s"
			, string_stream::c_str(texturec_output)
			);

		Buffer blob = opts.read_temporary(tex_out.c_str());
		opts.delete_file(tex_out.c_str());

		// Write DDS
		opts.write(RESOURCE_HEADER(RESOURCE_VERSION_TEXTURE));
		opts.write(array::size(blob));
		opts.write(blob);

		return 0;
	}

} // namespace texture_resource_internal
#endif // if CROWN_CAN_COMPILE

} // namespace crown
