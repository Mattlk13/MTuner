--
-- Copyright 2025 Milos Tosic. All rights reserved.
-- License: http://www.opensource.org/licenses/BSD-2-Clause
--

function projectDependencies_MTuner()
	return { "rmem", "rdebug", "rqt", "rg_memory" }
end

function projectExtraConfig_MTuner()
 	configuration { "*-gcc*" }
 		buildoptions {
			"-fopenmp",
 		}
	configuration { "osx" }
		defines { "QT_STRINGVIEW_LEVEL=2" }

	-- Retail: whole-program optimization / link-time code generation for the MTuner app code.
	-- Scoped to this project only (the static-lib dependencies are built without it).
	configuration { "vs*", "retail" }
		buildoptions { "/GL" }		-- whole-program optimization (compiler)
		linkoptions  { "/LTCG" }	-- link-time code generation (linker)
	configuration { "*-gcc*", "retail" }
		buildoptions { "-flto" }
		linkoptions  { "-flto" }

	configuration {}
		-- _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS silences MSVC C4996 (STL4043) emitted
		-- by concurrency::parallel_radixsort's use of stdext::unchecked_array_iterator.
		defines { "_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS" }
		includedirs	{ path.join(projectGetPath("MTuner"), "../") }
end

function projectExtraConfigExecutable_MTuner()
	includedirs	{ path.join(projectGetPath("MTuner"), "../") }

	if getTargetOS() == "linux" then
		links {
			"gomp",
		}
	end

	configuration {}
end

function projectAdd_MTuner()
	addProject_qt("MTuner")
end
