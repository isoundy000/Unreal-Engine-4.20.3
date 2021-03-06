// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class AppleMoviePlayer : ModuleRules
	{
        public AppleMoviePlayer(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicDependencyModuleNames.AddRange(
				new string[] {
					"Core",
				    "CoreUObject",
				    "Engine",
                    "MoviePlayer",
                    "RenderCore",
                    "RHI",
                    "Slate"
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"SlateCore",
				}
			);

			if (Target.Platform == UnrealTargetPlatform.Mac)
			{
				PublicFrameworks.AddRange(
					new string[] {
						"QuartzCore"
					}
				);
			}
		}
	}
}
