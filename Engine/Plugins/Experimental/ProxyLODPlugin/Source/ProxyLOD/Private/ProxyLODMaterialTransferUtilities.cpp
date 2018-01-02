// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.


#include "ProxyLODMaterialTransferUtilities.h"

#include "ProxyLODkDOPInterface.h"
#include "ProxyLODThreadedWrappers.h"

#include "ProxyLODMeshConvertUtils.h"
#include "Math/Matrix.h" // used in chirality..


namespace
{
	// returns the HitUV
	template<typename TestFunctorType>
	float EvaluateHit(const FVector& HitPoint, const uint32 SrcPolyId, const FRawMeshArrayAdapter& MeshAdapter,
		ProxyLOD::FSrcMeshData& HitPayload, TestFunctorType& TestFunctor)
	{


		int32 MeshId; int32 LocalFaceNumber;
		const FRawMeshArrayAdapter::FRawPoly SrcPoly = MeshAdapter.GetRawPoly(SrcPolyId, MeshId, LocalFaceNumber);
		int32 MaterialIdx = SrcPoly.FaceMaterialIndex;

		FVector2D UVs[3];
		for (int i = 0; i < 3; ++i) UVs[i] = SrcPoly.WedgeTexCoords[0][i];

		// Get Additional data about this poly - might have alternate UVs that we need to use
		const FMeshMergeData& MeshMergeData = MeshAdapter.GetMeshMergeData(MeshId);

		const bool bHasNewUVs = (MeshMergeData.NewUVs.Num() != 0);

		if (bHasNewUVs)
		{
			const TArray<FVector2D>& NewUVs = MeshMergeData.NewUVs;

			for (int32 i = 0, Offset = LocalFaceNumber * 3; i < 3; ++i) UVs[i] = NewUVs[Offset + i];
		}
		// might need to rescale old uvs instead.
		if (!bHasNewUVs && MeshMergeData.TexCoordBounds.IsValidIndex(MaterialIdx))
		{
			const FBox2D& Bounds = MeshMergeData.TexCoordBounds[MaterialIdx];
			if (Bounds.GetArea() > 0)
			{
				const FVector2D MinUV = Bounds.Min;
				const FVector2D ScaleUV(1.0f / (Bounds.Max.X - Bounds.Min.X), 1.0f / (Bounds.Max.Y - Bounds.Min.Y));


				for (int32 i = 0; i < 3; ++i)
				{
					// NB: FVector2D * FVector2D is componetwise.
					UVs[i] = (UVs[i] - MinUV) * ScaleUV;
				}
			}
		}

		HitPayload.MaterialId = MaterialIdx;
		HitPayload.TriangleId = SrcPolyId;

		// Record the triangle-local coordinates of this hit point.
		HitPayload.BarycentricCoords = ProxyLOD::ComputeBarycentricWeights(SrcPoly.VertexPositions, HitPoint);

		// Record the UV coordinates of the hit point.
		HitPayload.UV = ProxyLOD::InterpolateVertexData(HitPayload.BarycentricCoords, UVs);


		// Is the SrcNormal roughly in the same direction as the ray
		// the face normal of this poly
		return TestFunctor(SrcPoly);
	}

	float EvaluateHit(const FVector& HitPoint, const uint32 SrcPolyId, const FRawMeshArrayAdapter& MeshAdapter,
		ProxyLOD::FSrcMeshData& HitPayload)
	{
		auto NoOpFunctor = [](const FRawMeshArrayAdapter::FRawPoly& RawPoly)->float {return 1; };

		return EvaluateHit(HitPoint, SrcPolyId, MeshAdapter, HitPayload, NoOpFunctor);
	};

	// returns the HitUV
	template <bool Forward>
	float EvaluateHit(const FkHitResult& HitResult, const FVector& RayStart, const FVector& RayDirection, const FRawMeshArrayAdapter& MeshAdapter,
		ProxyLOD::FSrcMeshData& HitPayload)
	{

		// Get the Poly we hit

		const uint32 SrcPolyId = HitResult.Item;

		// Interpolate the UVs to the hit point

		const float  HitTime = (Forward) ? HitResult.Time : -HitResult.Time;
		const FVector HitPoint = RayStart + HitTime * RayDirection;

		auto DirectionalTest = [&RayDirection](const FRawMeshArrayAdapter::FRawPoly& RawPoly)->float
		{
			// Is the SrcNormal roughly in the same direction as the ray
			// the face normal of this poly
			// const FVector SrcNormal = ComputeNormal(RawPoly.VertexPositions); 

			FVector SrcNormal = RawPoly.WedgeTangentZ[0] + RawPoly.WedgeTangentZ[1] + RawPoly.WedgeTangentZ[2];
			SrcNormal.Normalize();

			return FVector::DotProduct(SrcNormal, RayDirection);
		};

		HitPayload.Forward = (Forward == true) ? 1 : 0;

		return EvaluateHit(HitPoint, SrcPolyId, MeshAdapter, HitPayload, DirectionalTest);
	}

}

// Generate a map between UV locations on the simplified geometry and points on the Src geometry
// Inactive texels in the result have MaterialId = -1;

ProxyLOD::FSrcDataGrid::Ptr ProxyLOD::CreateCorrespondence( const FRawMeshArrayAdapter& SrcMesh, 
															const FVertexDataMesh& ReducedMesh, 
															const ProxyLOD::FRasterGrid& UVGrid, 
															const int32 TransferType, 
															float MaxRayLength)
{
	const int32 TestTransferType = TransferType;

	// The dimensions of the resulting buffer match the UV Grid.

	const FIntPoint Size = UVGrid.Size();

	// Create the target

	ProxyLOD::FSrcDataGrid::Ptr TargetGridPtr = ProxyLOD::FSrcDataGrid::Create(Size.X, Size.Y);
	ProxyLOD::FSrcDataGrid& TargetGrid = *TargetGridPtr;

	// Construct a k-DOP tree that holds the src geometry.
	// This is used later when we have to form a correspondence between
	// simplified geometry and src geometry by shooting rays.

	// NB: This construction could be done earlier in parallel with the UV generation or simplification.

	FkDOPTree SrckDOPTree;
	BuildkDOPTree(SrcMesh, SrckDOPTree);
	FUnitTransformDataProvider kDOPDataProvider(SrckDOPTree);

	// Access to the simplified mesh data.

	const auto& Indices = ReducedMesh.Indices;
	const auto& Points = ReducedMesh.Points;
	const auto& Normal = ReducedMesh.Normal;


	// Iterate over the the UVGrid.

	ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, Size.Y),
		[&](const ProxyLOD::FIntRange& Range)
	{

		for (int j = Range.begin(); j < Range.end(); ++j)
		{
			for (int i = 0; i < Size.X; ++i)
			{
				const auto& TexelData = UVGrid(i, j);
				ProxyLOD::FSrcMeshData& TargetTexel = TargetGrid(i, j);
				TargetTexel.MaterialId = -1;

				if (TexelData.TriangleId > -1)
				{
					const uint32 TriangleId = TexelData.TriangleId;

					// The Barycentric Coords we identify with this texel.
					// NB: when Data.SignedDistance > 0 this is a point on the closest
					// triangle.

					// In order BarycentricCoords = distance from {V0-V1 edge,  V1-V2 edge, V2-V1 edge}.

					const auto& BarycentricCoords = TexelData.BarycentricCoords;

					// The vertices of the triangle in question.

					const uint32 Offset = TriangleId * 3;

					// The triangle in the simplified mesh that corresponds to this texel

					const FVector VertPos[3] = { Points[Indices[Offset + 0]], Points[Indices[Offset + 1]], Points[Indices[Offset + 2]] };
					const FVector VertNormal[3] = { Normal[Indices[Offset + 0]], Normal[Indices[Offset + 1]], Normal[Indices[Offset + 2]] };

					// Find the location in 3d space the corresponds to this UV on this triangle,
					// this will be the origin for rays we shoot.
					// NB: This is interpolating based on the texture-space barycentric-coords.  
					// This could introduce some distortion. 

					const FVector RayOrigin = ProxyLOD::InterpolateVertexData(BarycentricCoords, VertPos);

					// face normal.. used for testing
					//  FVector RayDirection = ComputeNormal(VertPos);

					// The averaged normal at this position, revert to face normal if this fails.
					FVector RayDirection = ProxyLOD::InterpolateVertexData(BarycentricCoords, VertNormal);
					bool bSuccess = RayDirection.Normalize(1.e-5); // Todo: what if this normalize fails?
					if (!bSuccess)
					{
						RayDirection = ComputeNormal(VertPos);
					}


					// Slight offset to the star point when casting rays to insure that we don't miss the case
					// when the Src geometry is actually co-planar with triangle we are shooting from.

					const FVector ForwardRayStart = RayOrigin - 0.0001 * RayDirection;
					const FVector ReverseRayStart = RayOrigin + 0.0001 * RayDirection;

					// We have to shoot rays in forward & back-facing normal direction to look for the closest source triangle. 
					// Construct a forward and a reverse ray.
					FVector ForwardRayEnd = ForwardRayStart + RayDirection * MaxRayLength;
					FVector ReverseRayEnd = ReverseRayStart - RayDirection * MaxRayLength;

					// Find t

					FkHitResult ForwardResult;

					TkDOPLineCollisionCheck<const FUnitTransformDataProvider, uint32>  ForwardRay(ForwardRayStart, ForwardRayEnd, true, kDOPDataProvider, &ForwardResult);

					FkHitResult ReverseResult;

					TkDOPLineCollisionCheck<const FUnitTransformDataProvider, uint32>  ReverseRay(ReverseRayStart, ReverseRayEnd, true, kDOPDataProvider, &ReverseResult);


					// Fire both rays

					bool bForwardHit = SrckDOPTree.LineCheck(ForwardRay);
					bool bReverseHit = SrckDOPTree.LineCheck(ReverseRay);


					//int32 HitTriIdx = -1;
					//int32 HitForward = -2;

					ProxyLOD::FSrcMeshData HitPayload;
					HitPayload.MaterialId = -1;
					HitPayload.UV = FVector2D(0.f, 0.f);
					if (TestTransferType == 0)
					{

						if (bForwardHit && bReverseHit) // Both directions hit.
						{


							ProxyLOD::FSrcMeshData ForwardHitPayload;
							const float ForwardHitNormalDotNormal = EvaluateHit<true>(ForwardResult, ForwardRayStart, RayDirection, SrcMesh, ForwardHitPayload);
							const bool bForwardCodirectional = (ForwardHitNormalDotNormal > 0);

							ProxyLOD::FSrcMeshData ReverseHitPayload;
							const float ReverseHitNormalDotNormal = EvaluateHit<false>(ReverseResult, ReverseRayStart, RayDirection, SrcMesh, ReverseHitPayload);
							const bool bReverseCodirectional = (ReverseHitNormalDotNormal > 0);

							// If either are co-aligned, we will use it.
							HitPayload.CoAligned = (bForwardCodirectional || bReverseCodirectional) ? 1 : -1;

							if (bForwardCodirectional && bReverseCodirectional) // Both hits were "valid"
							{

								// pick the closest
								if (ForwardResult.Time < ReverseResult.Time)
								{
									HitPayload = ForwardHitPayload;
									HitPayload.Forward = 1;
								}
								else
								{
									HitPayload = ReverseHitPayload;
									HitPayload.Forward = 0;
								}
							}
							else if (bForwardCodirectional) // only the forward hit was "valid"
							{
								HitPayload = ForwardHitPayload;
								HitPayload.Forward = 1;
							}
							else if (bReverseCodirectional) // only the reverse hit was "valid"
							{
								HitPayload = ReverseHitPayload;
								HitPayload.Forward = 0;
							}
							else
							{
								HitPayload = ReverseHitPayload;
								HitPayload.Forward = -1;
							}
						}
						else if (bForwardHit) // just forward hit
						{
							const float NormalDotNormal = EvaluateHit<true>(ForwardResult, ForwardRayStart, RayDirection, SrcMesh, HitPayload);
							HitPayload.CoAligned = (NormalDotNormal > 0) ? 1 : -1;
						}
						else if (bReverseHit) // just reverse hit
						{
							const float NormalDotNormal = EvaluateHit<false>(ReverseResult, ReverseRayStart, RayDirection, SrcMesh, HitPayload);
							HitPayload.CoAligned = (NormalDotNormal > 0) ? 1 : -1;
						}
					}// end test type 0
					else
					{
						float NormalDotNormal;

						// both directions hit.
						if (bForwardHit && bReverseHit)
						{
							// just pick the closest
							if (ForwardResult.Time < ReverseResult.Time)
							{
								NormalDotNormal = EvaluateHit<true>(ForwardResult, ForwardRayStart, RayDirection, SrcMesh, HitPayload);
							}
							else
							{
								NormalDotNormal = EvaluateHit<false>(ReverseResult, ReverseRayStart, RayDirection, SrcMesh, HitPayload);
							}
						}
						else if (bForwardHit)
						{
							NormalDotNormal = EvaluateHit<true>(ForwardResult, ForwardRayStart, RayDirection, SrcMesh, HitPayload);

						}
						else if (bReverseHit)
						{
							NormalDotNormal = EvaluateHit<false>(ReverseResult, ReverseRayStart, RayDirection, SrcMesh, HitPayload);
						}
						else
						{
							NormalDotNormal = 1;
						}
						HitPayload.CoAligned = (NormalDotNormal > 0.f) ? 1 : -1;

					}
					// 
					if (HitPayload.MaterialId != -1)
					{
						TargetTexel = HitPayload;
					}

				}

			}
		}
	});

	return TargetGridPtr;
}


// Generate a map between UV locations on the simplified geometry and points on the Src geometry
// Inactive texels in the result have MaterialId = -1;
ProxyLOD::FSrcDataGrid::Ptr ProxyLOD::CreateCorrespondence( const openvdb::Int32Grid& ClosestPolyGrid, 
															const FRawMeshArrayAdapter& SrcMesh,
															const FVertexDataMesh& ReducedMesh, 
															const ProxyLOD::FRasterGrid& UVGrid,
															const int32 TransferType, 
															float MaxRayLength)
{

	const int32 TestTransferType = TransferType;

	// The dimensions of the resulting buffer match the UV Grid.

	const FIntPoint Size = UVGrid.Size();

	// Create the target

	ProxyLOD::FSrcDataGrid::Ptr TargetGridPtr = ProxyLOD::FSrcDataGrid::Create(Size.X, Size.Y);
	ProxyLOD::FSrcDataGrid& TargetGrid = *TargetGridPtr;

	// Construct a k-DOP tree that holds the src geometry.
	// This is used later when we have to form a correspondence between
	// simplified geometry and src geometry by shooting rays.

	// NB: This construction could be done earlier in parallel with the UV generation or simplification.

	FkDOPTree SrckDOPTree;
	BuildkDOPTree(SrcMesh, SrckDOPTree);
	FUnitTransformDataProvider kDOPDataProvider(SrckDOPTree);

	// Access to the simplified mesh data.

	const auto& Indices = ReducedMesh.Indices;
	const auto& Points = ReducedMesh.Points;
	const auto& Normal = ReducedMesh.Normal;


	// Iterate over the the UVGrid.

	ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, Size.Y),
		[&](const ProxyLOD::FIntRange& Range)
	{

		// Accessor used to find the closest poly used in generating the isosurface

		openvdb::Int32Grid::ConstAccessor ClosestPolyIdAccessor = ClosestPolyGrid.getConstAccessor();
		const auto& XForm = ClosestPolyGrid.transform();


		for (int j = Range.begin(); j < Range.end(); ++j)
		{
			for (int i = 0; i < Size.X; ++i)
			{
				const auto& TexelData = UVGrid(i, j);
				ProxyLOD::FSrcMeshData& TargetTexel = TargetGrid(i, j);
				TargetTexel.MaterialId = -1;

				if (TexelData.TriangleId > -1)
				{
					const uint32 TriangleId = TexelData.TriangleId;

					// The Barycentric Coords we identify with this texel.
					// NB: when Data.SignedDistance > 0 this is a point on the closest
					// triangle.

					// In order BarycentricCoords = distance from {V0-V1 edge,  V1-V2 edge, V2-V1 edge}.

					const auto& BarycentricCoords = TexelData.BarycentricCoords;

					// The vertices of the triangle in question.

					const uint32 Offset = TriangleId * 3;

					// The triangle in the simplified mesh that corresponds to this texel

					const FVector VertPos[3] = { Points[Indices[Offset + 0]], Points[Indices[Offset + 1]], Points[Indices[Offset + 2]] };

					// Find the location in 3d space the corresponds to this UV on this triangle,
					// this will be the origin for rays we shoot.
					// NB: This is interpolating based on the texture-space barycentric-coords.  
					// This could introduce some distortion. 

					const FVector RayOrigin = ProxyLOD::InterpolateVertexData(BarycentricCoords, VertPos);


					ProxyLOD::FSrcMeshData HitPayload;
					HitPayload.MaterialId = -1;
					HitPayload.UV = FVector2D(0.f, 0.f);

					// Find the closest poly using the vdb index grid.  This should only return src polys that contributed
					// to the resulting iso-surface.

					openvdb::Int32 ClosestPolyId;
					const openvdb::Coord ijk = XForm.worldToIndexCellCentered(openvdb::Vec3d(RayOrigin.X, RayOrigin.Y, RayOrigin.Z));
					bool bFoundClosePoly = ClosestPolyIdAccessor.probeValue(ijk, ClosestPolyId);

					if (bFoundClosePoly)
					{

						// compute the barycentric coords of the projection of 'RayOrigin' onto the closest poly
						EvaluateHit(RayOrigin, ClosestPolyId, SrcMesh, HitPayload);
						HitPayload.Forward = -2;

					}
					else
					{


						const FVector VertNormal[3] = { Normal[Indices[Offset + 0]], Normal[Indices[Offset + 1]], Normal[Indices[Offset + 2]] };



						// Look for the closest poly.


						// face normal.. used for testing
						//  FVector RayDirection = ComputeNormal(VertPos);

						// The averaged normal at this position.
						FVector RayDirection = ProxyLOD::InterpolateVertexData(BarycentricCoords, VertNormal);
						RayDirection.Normalize(); // Todo: what if this normalize fails?



												  // Slight offset to the star point when casting rays to insure that we don't miss the case
												  // when the Src geometry is actually co-planar with triangle we are shooting from.

						const FVector ForwardRayStart = RayOrigin - 0.0001 * RayDirection;
						const FVector ReverseRayStart = RayOrigin + 0.0001 * RayDirection;

						// We have to shoot rays in forward & backfacing normal direction to look for the closest source triangle. 
						// Construct a forward and a reverse ray.
						FVector ForwardRayEnd = ForwardRayStart + RayDirection * MaxRayLength;
						FVector ReverseRayEnd = ReverseRayStart - RayDirection * MaxRayLength;

						// Find t

						FkHitResult ForwardResult;

						TkDOPLineCollisionCheck<const FUnitTransformDataProvider, uint32>  ForwardRay(ForwardRayStart, ForwardRayEnd, true, kDOPDataProvider, &ForwardResult);

						FkHitResult ReverseResult;

						TkDOPLineCollisionCheck<const FUnitTransformDataProvider, uint32>  ReverseRay(ReverseRayStart, ReverseRayEnd, true, kDOPDataProvider, &ReverseResult);


						// Fire both rays

						bool bForwardHit = SrckDOPTree.LineCheck(ForwardRay);
						bool bReverseHit = SrckDOPTree.LineCheck(ReverseRay);

						if (bForwardHit && bReverseHit) // Both directions hit.
						{
							ProxyLOD::FSrcMeshData ForwardHitPayload;
							const float ForwardHitNormalDotNormal = EvaluateHit<true>(ForwardResult, ForwardRayStart, RayDirection, SrcMesh, ForwardHitPayload);
							const bool bForwardCodirectional = (ForwardHitNormalDotNormal > 0);

							ProxyLOD::FSrcMeshData ReverseHitPayload;
							const float ReverseHitNormalDotNormal = EvaluateHit<false>(ReverseResult, ReverseRayStart, RayDirection, SrcMesh, ReverseHitPayload);
							const bool bReverseCodirectional = (ReverseHitNormalDotNormal > 0);


							if (bForwardCodirectional && bReverseCodirectional) // Both hits were "valid"
							{
								if (TestTransferType == 0)
								{
									// pick the closest.
									if (ForwardResult.Time < ReverseResult.Time)
									{
										HitPayload = ForwardHitPayload;
									}
									else
									{
										HitPayload = ReverseHitPayload;
									}

								}
								else
								{
									// pick forward
									HitPayload = ForwardHitPayload;
								}
							}
							else if (bForwardCodirectional) // only the forward hit was "valid"
							{
								// pick forward
								HitPayload = ForwardHitPayload;
							}
							else if (bReverseCodirectional) // only the reverse hit was "valid"
							{
								HitPayload = ReverseHitPayload;
							}
							else
							{
								HitPayload = ReverseHitPayload;
								HitPayload.Forward = -1;
							}
						}
						else if (bForwardHit)
						{
							EvaluateHit<true>(ForwardResult, ForwardRayStart, RayDirection, SrcMesh, HitPayload);
						}
						else if (bReverseHit)
						{
							EvaluateHit<false>(ReverseResult, ReverseRayStart, RayDirection, SrcMesh, HitPayload);
						}

					}

					// 
					if (HitPayload.MaterialId != -1)
					{
						TargetTexel = HitPayload;
					}

				}

			}
		}
	});

	return TargetGridPtr;
}


namespace
{
	template <EFlattenMaterialProperties PropertyType>
	void TransferMaterial(const ProxyLOD::FSrcDataGrid& CorrespondenceGrid, const ProxyLOD::FRasterGrid& UVGrid, const TArray<FFlattenMaterial>& InputMaterials, TArray<FLinearColor>& SamplesBuffer)
	{
		checkSlow(CorrespondenceGrid.Size() == UVGrid.Size());

		const FIntPoint Size = UVGrid.Size();
		const int32 BufferSize = Size.X * Size.Y;
		ResizeArray(SamplesBuffer, BufferSize);

		// Init buffer to zero color

		ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, BufferSize),
			[&SamplesBuffer](const ProxyLOD::FIntRange& Range)
		{
			FLinearColor* Data = SamplesBuffer.GetData();
			for (int32 i = Range.begin(), I = Range.end(); i < I; ++i) Data[i] = FLinearColor(0.f, 0.f, 0.f, 0.f);
		});


		// Iterate over the the UVGrid.

		ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, Size.Y),
			[&](const ProxyLOD::FIntRange& Range)
		{

			const FColor EmptySrcColor(0, 0, 255);

			auto GetColorFromBuffer = [](const FIntPoint& Size, const TArray<FColor>& Buffer, const FIntPoint& ij)->FLinearColor
			{
				FIntPoint SampleIJ = ij;
				// if the sample requested is outside the 2d array bounds, just shift it to the closest valid point. 
				if (SampleIJ.X > Size.X - 1) SampleIJ.X = Size.X - 1;
				if (SampleIJ.Y > Size.Y - 1) SampleIJ.Y = Size.Y - 1;
				if (SampleIJ.X < 0) SampleIJ.X = 0;
				if (SampleIJ.Y < 0) SampleIJ.Y = 0;

				FColor SampleColor = Buffer[SampleIJ.X + SampleIJ.Y * Size.X];

				return FLinearColor(SampleColor);
			};

			for (int32 j = Range.begin(); j < Range.end(); ++j)
			{

				for (int32 i = 0; i < Size.X; ++i)
				{

					const float MinFloat = std::numeric_limits<float>::lowest();

					// init the result with a non-valid number.  
					FLinearColor ResultLinearColor = FLinearColor(MinFloat, 0.f, 0.f, 0.f);

					const auto& DstTexelData = UVGrid(i, j);
					const int32 DstTriangleId = DstTexelData.TriangleId;
					const ProxyLOD::FSrcMeshData& CorrespondenceData = CorrespondenceGrid(i, j);

					if (DstTriangleId > -1)
					{

						if (CorrespondenceData.MaterialId == -1) continue;

						const int32 MaterialIdx = CorrespondenceData.MaterialId;

						FVector2D SrcUV = CorrespondenceData.UV;

						// Why is this happening?
						if (SrcUV.X > 1 || SrcUV.X < 0 || SrcUV.Y > 1 || SrcUV.Y < 0)
						{
							SrcUV = FVector2D(0.f, 0.f);
						}


						const FIntPoint SrcBufferSize = InputMaterials[MaterialIdx].GetPropertySize(PropertyType);
						const TArray<FColor>& SrcBuffer = InputMaterials[MaterialIdx].GetPropertySamples(PropertyType);

						// Look up the color at the hit point


						// bilinear interpolation of the color
						if (SrcBuffer.Num() > 1)
						{
							const FVector2D SrcTexelLocation(SrcUV.X * SrcBufferSize.X, SrcUV.Y * SrcBufferSize.Y);

							// 0,0 corner
							const FIntPoint Min(FMath::FloorToInt(SrcTexelLocation.X), FMath::FloorToInt(SrcTexelLocation.Y));

							// offset.
							const FVector2D Delta(SrcTexelLocation.X - (float)Min.X, SrcTexelLocation.Y - (float)Min.Y);

							// corner samples.
							FLinearColor Samples[4];
							Samples[0] = GetColorFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(0, 0));
							Samples[1] = GetColorFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(1, 0));
							Samples[2] = GetColorFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(0, 1));
							Samples[3] = GetColorFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(1, 1));

							FLinearColor BilinearInterp =
								Samples[0] * (1. - Delta.X) * (1. - Delta.Y) +
								Samples[1] * Delta.X * (1. - Delta.Y) +
								Samples[2] * (1. - Delta.X) * Delta.Y +
								Samples[3] * Delta.X * Delta.Y;

							// The interpolated color
							ResultLinearColor = BilinearInterp;
						}
						else if (SrcBuffer.Num() == 1)
						{
							//ResultColor = SrcBuffer[0];
							ResultLinearColor = FLinearColor(SrcBuffer[0]);

						}
						else // SrcBuffer.Num() == 0
						{
							//ResultColor = EmptySrcColor;
							ResultLinearColor = FLinearColor(EmptySrcColor);

						}
					}

					// Store the result.

					SamplesBuffer[i + j * Size.X] = ResultLinearColor;

				}
			}
		});
	}

	// Specialized color down-sample that also preserves average luma (computed with with Rec 709 standard)
	void DownSampleColor(const TArray<FLinearColor>& SuperSampleBuffer, const FIntPoint SuperSampleSize, TArray<FLinearColor>& ResultBuffer, const FIntPoint ResultSize)
	{
		const uint32 NumXSuperSamples = SuperSampleSize.X / ResultSize.X;
		const uint32 NumYSuperSamples = SuperSampleSize.Y / ResultSize.Y;

		checkSlow(SuperSampleSize.X == ResultSize.X * NumXSuperSamples);
		checkSlow(SuperSampleSize.Y == ResultSize.Y * NumYSuperSamples);

		const int32 BufferSize = ResultSize.X * ResultSize.Y;
		ResizeArray(ResultBuffer, BufferSize);

		// Note FLinearColor has a different way to define Luma.
		// the Rec 709 standard is dot(Color, float3(0.2126, 0.7152, 0.0722));
		const auto ComputeLuma = [](const FLinearColor& Color)->float
		{
			bool bUseRec709 = false;
			float Result;

			if (bUseRec709)
			{
				Result = Color.R * 0.2126 + Color.G * 0.7152 + Color.B  * 0.0722;
			}
			else
			{
				Result = Color.ComputeLuminance();
			}

			return Result;
		};

		// Gather

		ProxyLOD::Parallel_For(ProxyLOD::FUIntRange(0, ResultSize.Y),
			[&](const ProxyLOD::FUIntRange& Range)
		{
			for (uint32 j = Range.begin(), J = Range.end(); j < J; ++j)
			{
				uint32 joffset = j * NumYSuperSamples;
				for (uint32 i = 0; i < (uint32)ResultSize.X; ++i)
				{

					FLinearColor ResultColor(0, 0, 0, 0);
					float        Luma = 0;
					uint32       ResultCount = 0;

					uint32 ioffset = i * NumXSuperSamples;

					// loop over the super samples
					for (uint32 jj = joffset; jj < joffset + NumYSuperSamples; ++jj)
					{
						for (uint32 ii = ioffset; ii < ioffset + NumXSuperSamples; ++ii)
						{
							const FLinearColor& SuperSampleColor = SuperSampleBuffer[ii + jj * SuperSampleSize.X];
							if (SuperSampleColor.R != std::numeric_limits<float>::lowest())
							{
								ResultColor += SuperSampleColor;

								Luma += ComputeLuma(SuperSampleColor);
								ResultCount++;
							}

						}
					}


					// Average the result
					if (ResultCount > 0)
					{
						ResultColor = ResultColor / float(ResultCount);
						Luma = Luma / float(ResultCount);
						float TmpLuma = ComputeLuma(ResultColor);
						if (TmpLuma > 1.e-3)
						{
							float LumaCorrection = Luma / ComputeLuma(ResultColor);
							ResultColor *= LumaCorrection;
						}
					}

					// write it into the buffer
					ResultBuffer[i + j * ResultSize.X] = ResultColor;
				}
			}
		});
	}

	void DownSampleMaterial(const TArray<FLinearColor>& SuperSampleBuffer, const FIntPoint SuperSampleSize, TArray<FLinearColor>& ResultBuffer, const FIntPoint ResultSize)
	{
		const uint32 NumXSuperSamples = SuperSampleSize.X / ResultSize.X;
		const uint32 NumYSuperSamples = SuperSampleSize.Y / ResultSize.Y;

		checkSlow(SuperSampleSize.X == ResultSize.X * NumXSuperSamples);
		checkSlow(SuperSampleSize.Y == ResultSize.Y * NumYSuperSamples);

		const int32 BufferSize = ResultSize.X * ResultSize.Y;
		ResizeArray(ResultBuffer, BufferSize);

		// Gather

		ProxyLOD::Parallel_For(ProxyLOD::FUIntRange(0, ResultSize.Y),
			[&](const ProxyLOD::FUIntRange& Range)
		{
			for (uint32 j = Range.begin(), J = Range.end(); j < J; ++j)
			{
				uint32 joffset = j * NumYSuperSamples;
				for (uint32 i = 0; i < (uint32)ResultSize.X; ++i)
				{

					FLinearColor ResultColor(0, 0, 0, 0);
					uint32       ResultCount = 0;

					uint32 ioffset = i * NumXSuperSamples;

					// loop over the super samples
					for (uint32 jj = joffset; jj < joffset + NumYSuperSamples; ++jj)
					{
						for (uint32 ii = ioffset; ii < ioffset + NumXSuperSamples; ++ii)
						{
							const FLinearColor& SuperSampleColor = SuperSampleBuffer[ii + jj * SuperSampleSize.X];
							if (SuperSampleColor.R != std::numeric_limits<float>::lowest())
							{
								ResultColor += SuperSampleColor;
								ResultCount++;
							}

						}
					}


					// Average the result
					if (ResultCount > 0) ResultColor = ResultColor / float(ResultCount);

					// write it into the buffer
					ResultBuffer[i + j * ResultSize.X] = ResultColor;
				}
			}
		});
	}

	// Unpack a quantized vector into the [-1,1]x[-1,1]x[-1,1] cube
	FVector UnPackQuantizedVector(const FColor& QuantizedNormal)
	{
		FVector Vector(QuantizedNormal.R / 256.f, QuantizedNormal.G / 256.f, QuantizedNormal.B / 256.f);

		Vector = 2.f * Vector - FVector(1., 1., 1.);
		return Vector;
	}

	FColor QuantizeVector(const FVector& Vector)
	{
		FVector Tmp = 0.5 * (Vector + FVector(1., 1., 1.));
		FLinearColor LinearColor(Tmp);
		return LinearColor.Quantize();
	}

	// Specialized version for the normal map. 
	// This needs to convert the normal vector to the tangent space of the target mesh.
	// In the case that the source geometry didn't have a normal map, we have to generate one by 
	// directly sampling the geometry normals.

	void SuperSampleWSNormal(const FRawMeshArrayAdapter& SrcMeshAdapter,
		const ProxyLOD::FSrcDataGrid& SuperSampleCorrespondenceGrid,
		const ProxyLOD::FRasterGrid& SuperSampleUVGrid,
		const TArray<FFlattenMaterial>& InputMaterials,
		TArray<FVector>& SuperSampleBuffer)
	{

		typedef FVector FTangentSpace[3];

		//	const FIntPoint Size = OutMaterial.GetPropertySize(PropertyType);
		//	TArray<FColor>& TargetBuffer = OutMaterial.GetPropertySamples(PropertyType);
		const FIntPoint  BufferSize = SuperSampleUVGrid.Size();
		int32 BufferCount = BufferSize.X * BufferSize.Y;
		ResizeArray(SuperSampleBuffer, BufferCount);

		// Init buffer to zero color

		ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, BufferCount),
			[&SuperSampleBuffer](const ProxyLOD::FIntRange& Range)
		{
			FVector* Data = SuperSampleBuffer.GetData();
			for (int32 i = Range.begin(), I = Range.end(); i < I; ++i) Data[i] = FVector(0.f, 0.f, 0.f);
		});


		checkSlow(SuperSampleCorrespondenceGrid.Size() == SuperSampleUVGrid.Size());

		const FColor DefaultMissColor(255, 0, 0);
		const FColor EmptySrcColor(0, 0, 255);

		// Iterate over the the UVGrid.

		ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, BufferSize.Y),
			[&](const ProxyLOD::FIntRange& Range)
		{

			auto ComputeSrcTangentSpace = [&SrcMeshAdapter](const int32 TriangleId, const ProxyLOD::DArray3d& BarycentericCoords, FTangentSpace& TangentSpace)
			{

				int32 MeshId; int32 LocalFaceNumber;
				const FRawMeshArrayAdapter::FRawPoly SrcPoly = SrcMeshAdapter.GetRawPoly(TriangleId, MeshId, LocalFaceNumber);

				// Get the tangent space:  We try to compute this in a way that is consistent with LocalVertexFactory.usf (CalcTangentToLocal).

				TangentSpace[0] = ProxyLOD::InterpolateVertexData(BarycentericCoords, SrcPoly.WedgeTangentX);
				//TangentSpace[1] = ProxyLOD::InterpolateVertexData(BarycentericCoords, SrcPoly.WedgeTangentY);
				TangentSpace[2] = ProxyLOD::InterpolateVertexData(BarycentericCoords, SrcPoly.WedgeTangentZ);

				TangentSpace[2].Normalize();
				TangentSpace[0].Normalize();

				// bi-tangent
				TangentSpace[1] = FVector::CrossProduct(TangentSpace[2], TangentSpace[0]);

				// tangent
				TangentSpace[0] = FVector::CrossProduct(TangentSpace[1], TangentSpace[2]);
			};



			// Samples the normal map and unpacks the result
			auto GetNormalFromBuffer = [](const FIntPoint& Size, const TArray<FColor>& Buffer, const FIntPoint& ij)->FVector
			{
				FIntPoint SampleIJ = ij;
				// if the sample requested is outside the 2d array bounds, just shift it to the closest valid point. 
				if (SampleIJ.X > Size.X - 1) SampleIJ.X = Size.X - 1;
				if (SampleIJ.Y > Size.Y - 1) SampleIJ.Y = Size.Y - 1;
				if (SampleIJ.X < 0) SampleIJ.X = 0;
				if (SampleIJ.Y < 0) SampleIJ.Y = 0;

				// TangentSpace Normal
				FColor NormalAsColor = Buffer[SampleIJ.X + SampleIJ.Y * Size.X];

				// Map from uint8 0, 255 x 0,255 x 0,255  to -1,1 x -1,1 x -1,1 region
				FVector TSNormal = UnPackQuantizedVector(NormalAsColor);

				TSNormal.Normalize();
				return TSNormal;

			};

			// Iterate over the target texels.

			for (int32 j = Range.begin(); j < Range.end(); ++j)
			{

				for (int32 i = 0; i < BufferSize.X; ++i)
				{


					const float MinFloat = std::numeric_limits<float>::lowest();
					FVector& ResultNormal = SuperSampleBuffer[i + j * BufferSize.X];
					ResultNormal = FVector(MinFloat, 0.f, 0.f);


					const auto& SuperSampleTexelData = SuperSampleUVGrid(i, j);
					const ProxyLOD::FSrcMeshData& CorrespondenceData = SuperSampleCorrespondenceGrid(i, j);
					const int32 DstTriangleId = SuperSampleTexelData.TriangleId;

					if (DstTriangleId > -1)
					{
						const auto& DstBarycentricCoords = SuperSampleTexelData.BarycentricCoords;



						if (CorrespondenceData.MaterialId == -1) continue;

						const int32 MaterialIdx = CorrespondenceData.MaterialId;

						FVector2D SrcUV = CorrespondenceData.UV;

						const int32 SrcTriangleId = CorrespondenceData.TriangleId;
						const auto& SrcBarycentricCoords = CorrespondenceData.BarycentricCoords;
						FTangentSpace SrcTangentSpace;
						ComputeSrcTangentSpace(SrcTriangleId, SrcBarycentricCoords, SrcTangentSpace);

						// Why is this happening?
						if (SrcUV.X > 1 || SrcUV.X < 0 || SrcUV.Y > 1 || SrcUV.Y < 0)
						{
							SrcUV = FVector2D(0.f, 0.f);
						}


						const FIntPoint SrcBufferSize = InputMaterials[MaterialIdx].GetPropertySize(EFlattenMaterialProperties::Normal);
						const TArray<FColor>& SrcBuffer = InputMaterials[MaterialIdx].GetPropertySamples(EFlattenMaterialProperties::Normal);


						// By convention, a baked down src normal map will have a single element in the case that the original geoemetry had no normal map.

						const bool bSrcHasNormalMap = (SrcBuffer.Num() > 1);


						// Get world space samples of the normal - either from the normal map or directly from geometry.
						//  Sample the normal map in world space.  Use bilinear filtering.

						if (bSrcHasNormalMap) // testing.  We should just be encoding the geometry normal now.
						{

							const FVector2D SrcTexelLocation(SrcUV.X * SrcBufferSize.X, SrcUV.Y * SrcBufferSize.Y);

							// 0,0 corner

							const FIntPoint Min(FMath::FloorToInt(SrcTexelLocation.X), FMath::FloorToInt(SrcTexelLocation.Y));

							// offset.

							const FVector2D Delta(SrcTexelLocation.X - (float)Min.X, SrcTexelLocation.Y - (float)Min.Y);

							// corner samples.

							FVector NormalSampleArray[4];
							NormalSampleArray[0] = GetNormalFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(0, 0));
							NormalSampleArray[1] = GetNormalFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(1, 0));
							NormalSampleArray[2] = GetNormalFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(0, 1));
							NormalSampleArray[3] = GetNormalFromBuffer(SrcBufferSize, SrcBuffer, Min + FIntPoint(1, 1));

							// Interpolate the results

							const FVector TangentSpaceNormal =
								NormalSampleArray[0] * (1. - Delta.X) * (1. - Delta.Y) +
								NormalSampleArray[1] * Delta.X * (1. - Delta.Y) +
								NormalSampleArray[2] * (1. - Delta.X) * Delta.Y +
								NormalSampleArray[3] * Delta.X * Delta.Y;



							// Convert to worldspace 

							ResultNormal = TangentSpaceNormal.X * SrcTangentSpace[0] + TangentSpaceNormal.Y * SrcTangentSpace[1] + TangentSpaceNormal.Z * SrcTangentSpace[2];

							// If the src triangle we are sampling actually faces the opposite direction of the dst triangle, we assume the src poly was marked as double sided.

							//	ResultNormal[2] *= CorrespondenceData.CoAligned;

						}
						else // Src doesn't have NormalMap:   Sample the geometry normal in world space.
						{
							// We aren't filtering the normal from the geometry, but the fact that we 
							// are doing super-sampling should help average things out.

							ResultNormal = SrcTangentSpace[2];
						}


					}	// end if in triangle.					

				} // end i loop
			} // end j loop
		});
	}

	void DownSampleNormal(const TArray<FVector>& SuperSampledNormals, const FIntPoint SuperSampleSize,
		const ProxyLOD::FRasterGrid& DstUVGrid, const FRawMesh& DstRawMesh,
		TArray<FLinearColor>& DstBuffer, const FIntPoint DstBufferSize)
	{

		const uint32 NumXSuperSamples = SuperSampleSize.X / DstBufferSize.X;
		const uint32 NumYSuperSamples = SuperSampleSize.Y / DstBufferSize.Y;

		checkSlow(SuperSampleSize.X == DstBufferSize.X * NumXSuperSamples);
		checkSlow(SuperSampleSize.Y == DstBufferSize.Y * NumYSuperSamples);

		const int32 BufferSize = DstBufferSize.X * DstBufferSize.Y;
		ResizeArray(DstBuffer, BufferSize);

		// Gather

		ProxyLOD::Parallel_For(ProxyLOD::FUIntRange(0, DstBufferSize.Y),
			[&](const ProxyLOD::FUIntRange& Range)
		{
			// Store the tangent space in a 3x3 matrix.  

			typedef openvdb::Mat3R  FTangentSpace;


			auto ComputeDstTangentSpace = [&DstRawMesh](const int32 TriangleId, const ProxyLOD::DArray3d& BarycentericCoords, FTangentSpace& TangentSpace)
			{
				const int32 Indexes[3] = { TriangleId * 3, TriangleId * 3 + 1, TriangleId * 3 + 2 };

				// Compute the local tangent space
				FVector InterpolatedTangent;
				//{
				FVector TangentSamples[3];
				TangentSamples[0] = DstRawMesh.WedgeTangentX[Indexes[0]];
				TangentSamples[1] = DstRawMesh.WedgeTangentX[Indexes[1]];
				TangentSamples[2] = DstRawMesh.WedgeTangentX[Indexes[2]];
				InterpolatedTangent = ProxyLOD::InterpolateVertexData(BarycentericCoords, TangentSamples);
				//}

				FVector InterpolatedBiTangent;

				// The bi-tangent is reconstructed from the tangent and normal
				//{
				FVector BiTangentSamples[3];
				BiTangentSamples[0] = DstRawMesh.WedgeTangentY[Indexes[0]];
				BiTangentSamples[1] = DstRawMesh.WedgeTangentY[Indexes[1]];
				BiTangentSamples[2] = DstRawMesh.WedgeTangentY[Indexes[2]];
				InterpolatedBiTangent = ProxyLOD::InterpolateVertexData(BarycentericCoords, BiTangentSamples);
				//}

				FVector InterpolatedNormal;
				//{
				FVector NormalSamples[3];
				NormalSamples[0] = DstRawMesh.WedgeTangentZ[Indexes[0]];
				NormalSamples[1] = DstRawMesh.WedgeTangentZ[Indexes[1]];
				NormalSamples[2] = DstRawMesh.WedgeTangentZ[Indexes[2]];
				InterpolatedNormal = ProxyLOD::InterpolateVertexData(BarycentericCoords, NormalSamples);
				//}

				// Testing handedness. Is the tangent x bitangent pointing (roughly) in the direction of the normal?
				float Chirality[3];
				for (int32 i = 0; i < 3; ++i)
				{
					// StaticMeshVertexBuffer.h computes it this way...(see SetTangents)
					FMatrix Basis(
						FPlane(TangentSamples[i], 0),
						FPlane(BiTangentSamples[i], 0),
						FPlane(NormalSamples[i], 0),
						FPlane(0, 0, 0, 1)
					);
					Chirality[i] = (Basis.Determinant() < 0) ? -1.f : 1.f;
				}
				// NB: I don't know why this minus sign makes things work.. it shouldn't..
				float Handedness = FMath::Clamp(ProxyLOD::InterpolateVertexData(BarycentericCoords, Chirality), -1.f, 1.f);
				Handedness = (Handedness > 0) ? 1.f : -1.f;


				InterpolatedBiTangent = Handedness * FVector::CrossProduct(InterpolatedNormal, InterpolatedTangent);
				InterpolatedTangent = Handedness * FVector::CrossProduct(InterpolatedBiTangent, InterpolatedNormal);



				// Put the result in the column of a 3x3 matrix
				TangentSpace.setColumns(openvdb::Vec3f(InterpolatedTangent.X, InterpolatedTangent.Y, InterpolatedTangent.Z),
					openvdb::Vec3f(InterpolatedBiTangent.X, InterpolatedBiTangent.Y, InterpolatedBiTangent.Z),
					openvdb::Vec3f(InterpolatedNormal.X, InterpolatedNormal.Y, InterpolatedNormal.Z));

			};

			// Dotproduct between Vec3f and FVector

			auto LocalDot = [](const openvdb::Vec3f& VecA, const FVector& VecB)->float
			{
				return VecA[0] * VecB[0] + VecA[1] * VecB[1] + VecA[2] * VecB[2];
			};

			// Loop over the texels

			for (uint32 j = Range.begin(), J = Range.end(); j < J; ++j)
			{
				uint32 joffset = j * NumYSuperSamples;
				for (uint32 i = 0; i < (uint32)DstBufferSize.X; ++i)
				{
					// Encode this normal in terms of the local tangent space.
					const auto& DstTexel = DstUVGrid(i, j);

					FLinearColor& ResultColor = DstBuffer[i + j * DstBufferSize.X];
					ResultColor = FLinearColor(0, 0, 0, 0);

					if (DstTexel.TriangleId < 0) continue;

					FTangentSpace XForm;
					ComputeDstTangentSpace(DstTexel.TriangleId, DstTexel.BarycentricCoords, XForm);

					FVector      ResultVector(0, 0, 0);
					uint32       ResultCount = 0;

					uint32 ioffset = i * NumXSuperSamples;

					// loop over the super samples
					for (uint32 jj = joffset; jj < joffset + NumYSuperSamples; ++jj)
					{
						for (uint32 ii = ioffset; ii < ioffset + NumXSuperSamples; ++ii)
						{
							const FVector& SuperSampleColor = SuperSampledNormals[ii + jj * SuperSampleSize.X];
							if (SuperSampleColor.X != std::numeric_limits<float>::lowest())
							{
								const float DotWithWorldNormal = LocalDot(XForm.col(2), SuperSampleColor);
								ResultVector += SuperSampleColor / (DotWithWorldNormal * DotWithWorldNormal + 0.1);
								ResultCount++;
							}

						}
					}
					// Average the result
					if (ResultCount > 0)
					{
						ResultVector = ResultVector / float(ResultCount);
					}

					// Invert the tangent space
					if (XForm.det() > 0.001)
					{
						XForm = XForm.inverse();
					}
					else
					{
						XForm = XForm.transpose();
					}


					// project the result onto the tangent space vectors.
					// note: the matrix formulation doesn't assume that the local tangent space is orthonormal.

					openvdb::Vec3f Tmp(ResultVector.X, ResultVector.Y, ResultVector.Z);

					Tmp = XForm * Tmp;

					// if outside of the compression range, then normalize
					if (Tmp[0] < -1.f || Tmp[0] > 1.f || Tmp[1] < -1.f || Tmp[1] > 1.f || Tmp[2] < -1.f || Tmp[2] > 1.f)
					{
						Tmp.normalize();
					}


					//if (Tmp.lengthSqr() > 1.f) Tmp.normalize();
					Tmp = 0.5f * (Tmp + openvdb::Vec3f(1, 1, 1));

					//Tmp.normalize();
					// Re-center.
					// write it into the buffer
					ResultColor = FLinearColor(Tmp[0], Tmp[1], Tmp[2]);

				}
			}
		});

	}

	template <EFlattenMaterialProperties PropertyType>
	void TMapFlattenMaterial(const FRawMesh& DstRawMesh, const FRawMeshArrayAdapter& SrcMeshAdapter,
		const ProxyLOD::FSrcDataGrid&  SuperSampledCorrespondenceGrid,
		const ProxyLOD::FRasterGrid& SuperSampledDstUVGrid, const ProxyLOD::FRasterGrid& DstUVGrid,
		const TArray<FFlattenMaterial>& InputMaterials, FFlattenMaterial& OutMaterial)
	{
		const FIntPoint DstSize = OutMaterial.GetPropertySize(PropertyType);

		// Early out if no dst size has been allocated

		if (DstSize == FIntPoint::ZeroValue) return;

		TArray<FColor>& TargetBuffer = OutMaterial.GetPropertySamples(PropertyType);
		ResizeArray(TargetBuffer, DstSize.X * DstSize.Y);

		const int32 SampleCount = SuperSampledDstUVGrid.Size().X / DstSize.X;

		checkSlow(SampleCount == SuperSampledDstUVGrid.Size().Y / DstSize.Y);
		checkSlow(SuperSampledCorrespondenceGrid.Size() == SuperSampledDstUVGrid.Size());

		{
			// Sample into a linear color buffer.
			// Note, only the normal sampler uses the HighRes (MeshAdapter) and Simplified (VertexDataMesh) 
			// geometry. 

			TArray<FLinearColor> LinearColorBuffer;
			if (PropertyType == EFlattenMaterialProperties::Normal)
			{

				TArray<FVector> SuperSampleBuffer;
				SuperSampleWSNormal(SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, InputMaterials, SuperSampleBuffer);

				// Respect the local tangent spaces when down sampling the normal.
				DownSampleNormal(SuperSampleBuffer, SuperSampledDstUVGrid.Size(), DstUVGrid, DstRawMesh, LinearColorBuffer, DstSize);
			}
			else if (PropertyType == EFlattenMaterialProperties::Diffuse)
			{
				TArray<FLinearColor> SupperSampledMaterial;
				TransferMaterial<PropertyType>(SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, InputMaterials, SupperSampledMaterial);

				// Preserve the average luma when down sampling color.
				DownSampleColor(SupperSampledMaterial, SuperSampledDstUVGrid.Size(), LinearColorBuffer, DstSize);
			}
			else
			{

				TArray<FLinearColor> SupperSampledMaterial;
				TransferMaterial<PropertyType>(SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, InputMaterials, SupperSampledMaterial);

				// Generic down sample.
				DownSampleMaterial(SupperSampledMaterial, SuperSampledDstUVGrid.Size(), LinearColorBuffer, DstSize);

			}

			// Generate initial topology for dilation.

			ProxyLOD::TGrid<int32> TopologyGrid(DstSize.X, DstSize.Y);
			ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, DstSize.Y),
				[&DstSize, &TopologyGrid, &SuperSampledDstUVGrid, SampleCount](const ProxyLOD::FIntRange& Range)
			{

				for (int32 j = Range.begin(); j < Range.end(); ++j)
				{
					for (int32 i = 0; i < DstSize.X; ++i)
					{
						// will a triangle contribute to this super sampled texel?
						int32 ResultCounter = 0;
						for (int32 ii = i * SampleCount; ii < (i + 1) * SampleCount; ++ii)
						{
							for (int32 jj = j * SampleCount; jj < (j + 1) * SampleCount; ++jj)
							{
								const auto& TexelData = SuperSampledDstUVGrid(ii, jj);

								if (TexelData.TriangleId > -1) ResultCounter = 1;
							}
						}
						TopologyGrid(i, j) = ResultCounter;
					}
				}
			});

			// Dilate the linear color buffer

			bool bDilationRequired = true;
			while (bDilationRequired)
			{
				ProxyLOD::FLinearColorGrid  LinearColorGrid(LinearColorBuffer, DstSize);
				bDilationRequired = DilateGrid(LinearColorGrid, TopologyGrid);
			}


			// Transfer the result into the FColor buffer needed for output.

			// NB: the normal map was just quantized.
			if (PropertyType == EFlattenMaterialProperties::Normal)
			{
				ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, DstSize.X * DstSize.Y),
					[&LinearColorBuffer, &TargetBuffer](const ProxyLOD::FIntRange& Range)
				{
					const FLinearColor* LinearColorData = LinearColorBuffer.GetData();
					FColor* ColorData = TargetBuffer.GetData();

					for (int32 i = Range.begin(), I = Range.end(); i < I; ++i)
					{
						ColorData[i] = LinearColorData[i].QuantizeRound();
					}

				});
			}
			else
			{
				ProxyLOD::Parallel_For(ProxyLOD::FIntRange(0, DstSize.X * DstSize.Y),
					[&LinearColorBuffer, &TargetBuffer](const ProxyLOD::FIntRange& Range)
				{
					const FLinearColor* LinearColorData = LinearColorBuffer.GetData();
					FColor* ColorData = TargetBuffer.GetData();

					for (int32 i = Range.begin(), I = Range.end(); i < I; ++i)
					{
						ColorData[i] = LinearColorData[i].ToFColor(true);
					}

				});
			}

		}
	}




 // Map Diffuse, Specular, Metallic, Roughness, Normal, Emissive, Opacity to the correct materials.
 // NB: EFlattenMaterialProperties: SubSurface, OpacityMask and AmbientOcclusion are not included
void MapFlattenMaterial(const EFlattenMaterialProperties PropertyType, 
	const FRawMesh& DstRawMesh, 
	const FRawMeshArrayAdapter& SrcMeshAdapter,
	const ProxyLOD::FSrcDataGrid& SuperSampledCorrespondenceGrid, 
	const ProxyLOD::FRasterGrid& SuperSampledDstUVGrid,
	const ProxyLOD::FRasterGrid& DstUVGrid, 
	const TArray<FFlattenMaterial>& InputMaterials, 
	FFlattenMaterial& OutMaterial)
{
	switch (PropertyType)
	{
	case EFlattenMaterialProperties::Diffuse:
		TMapFlattenMaterial<EFlattenMaterialProperties::Diffuse>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	case EFlattenMaterialProperties::Specular:
		TMapFlattenMaterial<EFlattenMaterialProperties::Specular>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	case EFlattenMaterialProperties::Metallic:
		TMapFlattenMaterial<EFlattenMaterialProperties::Metallic>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	case EFlattenMaterialProperties::Roughness:
		TMapFlattenMaterial<EFlattenMaterialProperties::Roughness>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	case EFlattenMaterialProperties::Normal:
		TMapFlattenMaterial<EFlattenMaterialProperties::Normal>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	case EFlattenMaterialProperties::Emissive:
		TMapFlattenMaterial<EFlattenMaterialProperties::Emissive>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	case EFlattenMaterialProperties::Opacity:
		TMapFlattenMaterial<EFlattenMaterialProperties::Opacity>(DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		break;
	default:
		return;
		// do nothing.  
	};
}

}// end anonymous namespace


void ProxyLOD::MapFlattenMaterials(const FRawMesh& DstRawMesh, const FRawMeshArrayAdapter& SrcMeshAdapter,
	const ProxyLOD::FSrcDataGrid& SuperSampledCorrespondenceGrid,
	const ProxyLOD::FRasterGrid& SuperSampledDstUVGrid,
	const ProxyLOD::FRasterGrid& DstUVGrid,
	const TArray<FFlattenMaterial>& InputMaterials, 
	FFlattenMaterial& OutMaterial)
{

	ProxyLOD::FTaskGroup TaskGroup;
	for (int32 i = 0; i < (int32)EFlattenMaterialProperties::NumFlattenMaterialProperties; ++i)
	{
		EFlattenMaterialProperties MaterialProperty = (EFlattenMaterialProperties)i;

		// NB: The MaterialProperty is captured by value

		auto MapTask = [MaterialProperty, &DstRawMesh, &SrcMeshAdapter, &SuperSampledCorrespondenceGrid, &SuperSampledDstUVGrid, &DstUVGrid, &InputMaterials, &OutMaterial]
		()
		{
			// This is also threaded internally. 

			MapFlattenMaterial(MaterialProperty, DstRawMesh, SrcMeshAdapter, SuperSampledCorrespondenceGrid, SuperSampledDstUVGrid, DstUVGrid, InputMaterials, OutMaterial);
		};

		// enqueue the task with the task manager.

		TaskGroup.Run(MapTask);
	}

	// This does the dispatch and waits to join

	TaskGroup.Wait();
}

// Maps each triangle id to a color.  This is just for testing
void ProxyLOD::ColorMapFlattedMaterials( const FVertexDataMesh& VertexDataMesh, 
	                                     const FRawMeshArrayAdapter& MeshAdapter, 
	                                     const ProxyLOD::FRasterGrid& UVGrid, 
	                                     const TArray<FFlattenMaterial>& InputMaterials, 
	                                     FFlattenMaterial& OutMaterial)
{
	// testing - coloring the simplified mesh by the partitions generated by uvatlas
	FColor Range[13] = { FColor(255, 0, 0), FColor(0, 255, 0), FColor(0, 0, 255), FColor(255, 255, 0), FColor(0, 255, 255),
		FColor(153, 102, 0), FColor(249, 129, 162), FColor(29, 143, 177), FColor(118, 42, 145),
		FColor(255, 121, 75), FColor(102, 204, 51), FColor(153, 153, 255), FColor(255, 255, 255) };


	TArray<FColor>& ColorBuffer = OutMaterial.GetPropertySamples(EFlattenMaterialProperties::Diffuse);
	FIntPoint Size = OutMaterial.GetPropertySize(EFlattenMaterialProperties::Diffuse);
	ResizeArray(ColorBuffer, Size.X * Size.Y);


	for (int j = 0; j < Size.Y; ++j)
	{
		for (int i = 0; i < Size.X; ++i)
		{
			const auto& Data = UVGrid(i, j);
			if (Data.TriangleId > -1 || Data.SignedDistance < 0.)
			{
				uint32 TriangleId = Data.TriangleId;
				FLinearColor TriangleColor(Range[TriangleId % 13]);
				// intensity modulated by barycentric coords
				ProxyLOD::DArray3d BCoords = Data.BarycentricCoords;
				float Scale = (float)FMath::Min(FMath::Min(BCoords[0], BCoords[1]), BCoords[2]);
				TriangleColor *= Scale;
				ColorBuffer[i + j * Size.X] = TriangleColor.ToFColor(true);
			}
			else
			{
				ColorBuffer[i + j * Size.X] = FColor(0, 0, 0);
			}
		}
	}

	TArray<FColor> NormalMap = OutMaterial.GetPropertySamples(EFlattenMaterialProperties::Normal);

	NormalMap = InputMaterials[0].GetPropertySamples(EFlattenMaterialProperties::Normal);
}

void ProxyLOD::CopyFlattenMaterial( const FFlattenMaterial& InMaterial, 
	                                FFlattenMaterial& OutMaterial)
{
	for (int32 i = 0; i < (int32)EFlattenMaterialProperties::NumFlattenMaterialProperties; ++i)
	{
		EFlattenMaterialProperties Property = (EFlattenMaterialProperties)i;
		if (InMaterial.DoesPropertyContainData(Property))
		{

			if (InMaterial.IsPropertyConstant(Property)) // copy into a full sized buffer.
			{
				TArray<FColor>& OutBuffer = OutMaterial.GetPropertySamples(Property);
				FIntPoint Size = OutMaterial.GetPropertySize(Property);
				ResizeArray(OutBuffer, Size.X * Size.Y);

				const TArray<FColor>& InBuffer = InMaterial.GetPropertySamples(Property);

				for (int32 j = 0; j < Size.X * Size.Y; j++)
				{
					OutBuffer[j] = InBuffer[0];
				}
			}
			else
			{
				TArray<FColor>& OutBuffer = OutMaterial.GetPropertySamples(Property);
				FIntPoint Size = OutMaterial.GetPropertySize(Property);
				ResizeArray(OutBuffer, Size.X * Size.Y);

				const TArray<FColor>& InBuffer = InMaterial.GetPropertySamples(Property);

				for (int32 j = 0; j < Size.X * Size.Y; j++)
				{
					OutBuffer[j] = InBuffer[j];
				}
			}
		}
	}
}