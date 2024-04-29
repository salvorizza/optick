project "OptickCore"
	kind "StaticLib"
	language "C++"
    staticruntime "off"
	
	systemversion "10.0.15063.0"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
	
	defines { "_CRT_SECURE_NO_WARNINGS", "OPTICK_LIB=1" }
	defines { "OPTICK_EXPORTS" }
	defines { "OPTICK_ENABLE_GPU_D3D12=0" }
	defines { "OPTICK_ENABLE_GPU_VULKAN=0" }

	includedirs
	{
		"src"
	}
	
	files {
		"src/**.cpp",
        "src/**.h", 
	}
	
	vpaths {
		["api"] = { 
			"src/optick.h",
			"src/optick.config.h",
		},
	}

	filter "system:windows"
		systemversion "latest"
		cppdialect "C++17"

	filter "system:linux"
		pic "On"
		systemversion "latest"
		cppdialect "C++17"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
