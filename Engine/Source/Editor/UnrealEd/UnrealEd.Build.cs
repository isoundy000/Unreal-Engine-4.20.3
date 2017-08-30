// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class UnrealEd : ModuleRules
{
	public UnrealEd(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivatePCHHeaderFile = "Private/UnrealEdPrivatePCH.h";

		SharedPCHHeaderFile = "Public/UnrealEdSharedPCH.h";

		PrivateIncludePaths.AddRange(
			new string[] 
			{
				"Editor/UnrealEd/Private",
				"Editor/UnrealEd/Private/Settings",
				"Editor/PackagesDialog/Public",				
				"Developer/DerivedDataCache/Public",
				"Developer/TargetPlatform/Public",
			}
		);

		PrivateIncludePathModuleNames.AddRange(
			new string[] 
			{
				"BehaviorTreeEditor",
				"ClassViewer",
				"ContentBrowser",
				"DerivedDataCache",
				"DesktopPlatform",
				"LauncherPlatform",
				"EnvironmentQueryEditor",
				"GameProjectGeneration",
				"ProjectTargetPlatformEditor",
				"ImageWrapper",
				"MainFrame",
				"MaterialEditor",
				"MergeActors",
				"MeshUtilities",
				"Messaging",
				"MovieSceneCapture",
				"NiagaraEditor",
				"PlacementMode",
				"Settings",
				"SettingsEditor",
				"AudioEditor",
				"ViewportSnapping",
				"SourceCodeAccess",
				"ReferenceViewer",
				"IntroTutorials",
				"OutputLog",
				"Landscape",
				"Niagara",
				"SizeMap",
				"LocalizationService",
				"HierarchicalLODUtilities",
				"MessagingRpc",
				"PortalRpc",
				"PortalServices",
				"BlueprintNativeCodeGen",
				"ViewportInteraction",
				"VREditor",
				"Persona",
				"ClothingSystemEditorInterface",
            }
		);

		PublicDependencyModuleNames.AddRange(
			new string[] 
			{
				"BspMode",
				"Core",
				"CoreUObject",
				"DirectoryWatcher",
				"Documentation",
				"Engine",
				"Json",
				"Projects",
				"SandboxFile",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"SourceControl",
				"UnrealEdMessages",
				"AIModule",
				"GameplayDebugger",
				"BlueprintGraph",
				"Http",
				"UnrealAudio",
				"FunctionalTesting",
				"AutomationController",
				"Localization",
				"AudioEditor",
				"NetworkFileSystem",
                "UMG",
                "MeshDescription",
                "MeshBuilder",
            }
		);

		PrivateDependencyModuleNames.AddRange(
			new string[] 
			{
				"AssetRegistry",
				"LevelSequence",
				"AnimGraph",
				"AppFramework",
				"BlueprintGraph",
				"CinematicCamera",
				"DesktopPlatform",
				"LauncherPlatform",
				"EditorStyle",
				"EngineSettings",
				"InputCore",
				"InputBindingEditor",
				"LauncherServices",
				"MaterialEditor",
				"MessageLog",
				"PakFile",
				"PropertyEditor",
				"Projects",
				"RawMesh",
				"RenderCore", 
				"RHI", 
				"ShaderCore", 
				"Sockets",
				"SourceControlWindows",
				"StatsViewer",
				"SwarmInterface",
				"TargetPlatform",
				"TargetDeviceServices",
				"EditorWidgets",
				"GraphEditor",
				"Kismet",
				"InternationalizationSettings",
				"JsonUtilities",
				"Landscape",
				"HeadMountedDisplay",
				"MeshPaint",
				"MeshPaintMode",
				"Foliage",
				"VectorVM",
				"TreeMap",
				"MaterialUtilities",
				"Localization",
				"LocalizationService",
				"AddContentDialog",
				"GameProjectGeneration",
				"HierarchicalLODUtilities",
				"Analytics",
				"AnalyticsET",
				"PluginWarden",
				"PixelInspectorModule",
				"MovieScene",
				"MovieSceneTracks",
				"ViewportInteraction",
				"VREditor",
				"ClothingSystemEditor",
				"ClothingSystemRuntime",
				"ClothingSystemRuntimeInterface",
				"PIEPreviewDeviceProfileSelector",
            }
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[] 
			{
				"FontEditor",
				"StaticMeshEditor",
				"TextureEditor",
				"Cascade",
				"UMGEditor",
				"Matinee",
				"AssetTools",
				"ClassViewer",
				"CollectionManager",
				"ContentBrowser",
				"CurveTableEditor",
				"DataTableEditor",
				"DestructibleMeshEditor",
				"EditorSettingsViewer",
				"LandscapeEditor",
				"KismetCompiler",
				"DetailCustomizations",
				"ComponentVisualizers",
				"MainFrame",
				"LevelEditor",
				"PackagesDialog",
				"Persona",
				"PhAT",
				"ProjectLauncher",
				"DeviceManager",
				"SettingsEditor",
				"SessionFrontend",
				"Sequencer",
				"StringTableEditor",
				"GeometryMode",
				"TextureAlignMode",
				"FoliageEdit",
				"ImageWrapper",
				"Blutility",
				"IntroTutorials",
				"WorkspaceMenuStructure",
				"PlacementMode",
				"NiagaraEditor",
				"MeshUtilities",
				"MergeActors",
				"ProjectSettingsViewer",
				"ProjectTargetPlatformEditor",
				"PListEditor",
				"BehaviorTreeEditor",
				"EnvironmentQueryEditor",
				"ViewportSnapping",
				"GameplayTasksEditor",
				"UndoHistory",
				"SourceCodeAccess",
				"ReferenceViewer",
				"HotReload",
				"IOSPlatformEditor",
				"HTML5PlatformEditor",
				"SizeMap",
				"PortalProxies",
				"PortalServices",
				"GeometryCacheEd",
				"BlueprintNativeCodeGen",
				"OverlayEditor",
				"AnimationModifiers",
				"ClothPainter",
            }
		);

		if (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Win32 || Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.Linux)
		{
			DynamicallyLoadedModuleNames.Add("AndroidPlatformEditor");
		}

		CircularlyReferencedDependentModules.AddRange(
			new string[] 
			{
				"GraphEditor",
				"Kismet",
				"AudioEditor",
				"ViewportInteraction",
				"VREditor"
			}
		); 


		// Add include directory for Lightmass
		PublicIncludePaths.Add("Programs/UnrealLightmass/Public");

		PublicIncludePathModuleNames.AddRange(
			new string[] {
				"CollectionManager",
				"BlueprintGraph",
				"AddContentDialog",                
				"MeshUtilities",
                "AssetTools",
				"KismetCompiler",
			}
			);

		if ((Target.Platform == UnrealTargetPlatform.Win64) ||
			(Target.Platform == UnrealTargetPlatform.Win32))
		{
			PublicDependencyModuleNames.Add("XAudio2");
			PublicDependencyModuleNames.Add("AudioMixerXAudio2");
			PublicDependencyModuleNames.Add("UnrealAudioXAudio2");

			AddEngineThirdPartyPrivateStaticDependencies(Target, 
				"UEOgg",
				"Vorbis",
				"VorbisFile",
				"DX11Audio"
				);

			
		}

		if (Target.Platform == UnrealTargetPlatform.HTML5)
		{
			PublicDependencyModuleNames.Add("ALAudio");
		}

		AddEngineThirdPartyPrivateStaticDependencies(Target,
			"HACD",
			"VHACD",
			"FBX",
			"FreeType2"
		);

		SetupModulePhysXAPEXSupport(Target);

		if (UEBuildConfiguration.bCompileRecast)
		{
			PrivateDependencyModuleNames.Add("Navmesh");
			Definitions.Add( "WITH_RECAST=1" );
		}
		else
		{
			Definitions.Add( "WITH_RECAST=0" );
		}
	}
}