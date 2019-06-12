using UnrealBuildTool;
using System.IO;
using System;

public class V8 : ModuleRules
{
    protected string ThirdPartyPath
    {
        get { return Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty")); }
    }

    public int[] GetChakraCoreVersion()
    {
        string[] VersionHeader = Utils.ReadAllText(Path.Combine(ThirdPartyPath, "chakracore", "include", "ChakraCoreVersion.h")).Replace("\r\n", "\n").Replace("\t", " ").Split('\n');
        string VersionMajor = "0";
        string VersionMinor = "0";
        string VersionPatch = "0";
        foreach (string Line in VersionHeader)
        {
            if (Line.StartsWith("#define CHAKRA_CORE_MAJOR_VERSION"))
            {
                VersionMajor = Line.Split(' ')[2];
            }
            else if (Line.StartsWith("#define CHAKRA_CORE_MINOR_VERSION "))
            {
                VersionMinor = Line.Split(' ')[2];
            }
            else if (Line.StartsWith("#define CHAKRA_CORE_PATCH_VERSION "))
            {
                VersionPatch = Line.Split(' ')[2];
            }
        }
        return new int[] { Int32.Parse(VersionMajor), Int32.Parse(VersionMinor), Int32.Parse(VersionPatch) };
    }

    public V8(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateIncludePaths.AddRange(new string[]
        {
            Path.Combine(ThirdPartyPath, "chakracore", "include"),
            Path.Combine("V8", "Private")
        });

        PublicIncludePaths.AddRange(new string[]
        {
           // Path.Combine("V8", "Public")
        });

        PublicDependencyModuleNames.AddRange(new string[] 
        { 
            "Core", "CoreUObject", "Engine", "Sockets", "ApplicationCore", "UMG", "NavigationSystem"
        });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateIncludePaths.Add(Path.Combine(ThirdPartyPath, "chakracore-debug", "include"));
        }

        if (Target.bBuildEditor)
        {
            PublicDependencyModuleNames.AddRange(new string[]
            {
                "DirectoryWatcher"
            });
        }

        PrivateDependencyModuleNames.AddRange(new string[] 
        { 
            "libWebSockets",
            "ICU",
        });

        HackWebSocketIncludeDir(Target);

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] 
            { 
                "UnrealEd"
            });
        }

        // prevent C4668, -Wundef
        bEnableUndefinedIdentifierWarnings = false;

        LoadV8(Target);
    }

    private void HackWebSocketIncludeDir(ReadOnlyTargetRules Target)
    {
        string WebsocketPath = Path.Combine(Target.UEThirdPartySourceDirectory, "libWebSockets", "libwebsockets");
        string PlatformSubdir = (Target.Platform == UnrealTargetPlatform.HTML5 && Target.Architecture == "-win32") ? "Win32" :
        	Target.Platform.ToString();
        
        if (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Win32 ||
			(Target.Platform == UnrealTargetPlatform.HTML5 && Target.Architecture == "-win32"))
        {
            PlatformSubdir = Path.Combine(PlatformSubdir, Target.WindowsPlatform.GetVisualStudioCompilerVersionName());
		}        

        //PrivateIncludePaths.Add(Path.Combine(WebsocketPath, "include"));
        //PrivateIncludePaths.Add(Path.Combine(WebsocketPath, "include", PlatformSubdir));
		if (Target.Platform == UnrealTargetPlatform.Linux)
        {
			string platform = "/Linux/" + Target.Architecture;
			string IncludePath = WebsocketPath + "/include" + platform;
			
            PrivateIncludePaths.Add(WebsocketPath + "include/");
			PrivateIncludePaths.Add(IncludePath);
        }
    }

    private bool LoadV8(ReadOnlyTargetRules Target)
    {
        int[] node_version = GetChakraCoreVersion();
        bool ShouldLink_libsampler = !(node_version[0] == 5 && node_version[1] < 3);

        if ((Target.Platform == UnrealTargetPlatform.Win64) || (Target.Platform == UnrealTargetPlatform.Win32))
        {
            string LibrariesPath = Path.Combine(ThirdPartyPath, "chakracore", "lib");
            string DebuggerLibPath = Path.Combine(ThirdPartyPath, "chakracore-debug", "lib");

            if (Target.Platform == UnrealTargetPlatform.Win64)
            {
                LibrariesPath = Path.Combine(LibrariesPath, "Win64");
                DebuggerLibPath = Path.Combine(DebuggerLibPath, "Win64");
            }
            else
            {
                LibrariesPath = Path.Combine(LibrariesPath, "Win32");
                DebuggerLibPath = Path.Combine(DebuggerLibPath, "Win32");
            }

            if (Target.Configuration == UnrealTargetConfiguration.Debug || Target.Configuration == UnrealTargetConfiguration.DebugGame)
            {
                PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "Debug", "ChakraCore.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "ChakraCore.Debugger.Protocol.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "ChakraCore.Debugger.ProtocolHandler.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "ChakraCore.Debugger.Service.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "libboost_date_time-vc141-mt-x64-1_69.lib"));
            }
            else
            {
                PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "Release", "ChakraCore.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "ChakraCore.Debugger.Protocol.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "ChakraCore.Debugger.ProtocolHandler.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "ChakraCore.Debugger.Service.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(DebuggerLibPath, "Release", "libboost_date_time-vc141-mt-x64-1_69.lib"));
            }

            if (node_version[0] >= 6)
            {
                //PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "v8_init.lib"));
                //PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "v8_initializers.lib"));
            }

            if (ShouldLink_libsampler)
            {
                //PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "v8_libsampler.lib"));
            }

            PublicDefinitions.Add(string.Format("WITH_CHAKRA_CORE=1"));

            return true;
        }
        else if (Target.Platform == UnrealTargetPlatform.Android)
        {
            string LibrariesPath = Path.Combine(ThirdPartyPath, "chakracore", "lib", "Android");

            PublicLibraryPaths.Add(Path.Combine(LibrariesPath, "ARMv7"));
            PublicLibraryPaths.Add(Path.Combine(LibrariesPath, "ARM64"));
            PublicAdditionalLibraries.Add("ChakraCoreStatic");
            PublicDefinitions.Add(string.Format("WITH_CHAKRA_CORE=1"));

            return true;
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            string LibrariesPath = Path.Combine(ThirdPartyPath, "v8", "lib", "Linux");
            PublicLibraryPaths.Add(Path.Combine(LibrariesPath, "x64"));

            PublicAdditionalLibraries.Add("v8_base");
            PublicAdditionalLibraries.Add("v8_libbase");
            PublicAdditionalLibraries.Add("v8_libplatform");
            PublicAdditionalLibraries.Add("v8_nosnapshot");

            if (node_version[0] >= 6)
            {
                PublicAdditionalLibraries.Add("v8_init");
                PublicAdditionalLibraries.Add("v8_initializers");
            }

            if (ShouldLink_libsampler)
            {
                PublicAdditionalLibraries.Add("v8_libsampler");
            }

            PublicDefinitions.Add(string.Format("WITH_CHAKRA_CORE=1"));

            return true;
        }
        else if (Target.Platform == UnrealTargetPlatform.IOS)
        {
            string LibrariesPath = Path.Combine(ThirdPartyPath, "chakracore", "lib", "IOS");
            PublicLibraryPaths.Add(LibrariesPath);

            PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "libChakraCoreStatic.a"));
            PublicDefinitions.Add("WITH_CHAKRA_CORE=1");

            return true;
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string LibrariesPath = Path.Combine(ThirdPartyPath, "chakracore", "lib", "Mac");
            PublicLibraryPaths.Add(LibrariesPath);

            if (Target.Configuration == UnrealTargetConfiguration.Debug || Target.Configuration == UnrealTargetConfiguration.DebugGame)
            {
                PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "libChakraCoreStaticD.a"));
            }
            else
            {
                PublicAdditionalLibraries.Add(Path.Combine(LibrariesPath, "libChakraCoreStatic.a"));
            }

            PublicDefinitions.Add(string.Format("WITH_CHAKRA_CORE=1"));

            return true;
        }
        PublicDefinitions.Add(string.Format("WITH_CHAKRA_CORE=0"));
        return false;
    }
}
