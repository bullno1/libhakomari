local function add_flags(flags)
	buildoptions(flags)
	linkoptions(flags)
end

solution "lip"
	location "build"
	configurations { "Develop" }
	targetdir "bin"

	includedirs {
		"include",
		"deps",
	}
	defines { "_CRT_SECURE_NO_WARNINGS" }
	flags {
		"StaticRuntime",
		"Symbols",
		"NoEditAndContinue",
		"NoNativeWChar",
		"NoManifest",
		"OptimizeSpeed",
		"ExtraWarnings",
		"FatalWarnings"
	}

	configuration "linux"
		buildoptions { "-Wno-missing-field-initializers" }

		newoption {
			trigger = "with-asan",
			description = "Compile with AddressSanitizer"
		}

		if _OPTIONS["with-asan"] then
			add_flags { "-fsanitize=address" }
		end

		newoption {
			trigger = "with-ubsan",
			description = "Compile with UndefinedBehaviorSanitizer"
		}

		if _OPTIONS["with-ubsan"] then
			add_flags {
				"-fsanitize=undefined",
				"-fno-sanitize-recover=undefined"
			}
		end

		newoption {
			trigger = "with-clang",
			description = "Compile with Clang"
		}

		if _OPTIONS["with-clang"] then
			premake.gcc.cc = "clang"
			premake.gcc.cxx = "clang++"
			premake.gcc.llvm = true
		end

	project "cmp"
		language "C"
		kind "StaticLib"
		files {
			"deps/cmp/cmp.h",
			"deps/cmp/cmp.c"
		}

	project "hakomari"
		language "C"
		kind "StaticLib"
		files {
			"src/hakomari.c"
		}
		configuration "windows"
			links {
				"cmp",
				"serialport"
			}

	project "aya"
		language "C"
		kind "ConsoleApp"
		links { "hakomari", "SDL2" }
		files {
			"src/aya.c"
		}

		configuration "linux"
			links {
				"cmp",
				"serialport"
			}
