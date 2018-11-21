#include "StreetMapImporting.h"
#include "StreetMapComponent.h"
#include "StreetMapRoad.h"

#include "StreetMapSplineTools.h"

#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "StreetMapImporting"

static void BuildStreetMapRoads(class UStreetMapComponent* StreetMapComponent,
	const FStreetMapRoadBuildSettings& BuildSettings,
	const FStreetMapMeshBuildSettings& MeshBuildSettings)
{
	const FTransform LandscapeToWorld = BuildSettings.Landscape->ActorToWorld();
	const FVector SplineScaleXYZ = FVector(1.0f) / LandscapeToWorld.GetScale3D();
	ULandscapeSplinesComponent* SplinesComponent = FStreetMapSplineTools::ConditionallyCreateSplineComponent(BuildSettings.Landscape, SplineScaleXYZ);

	FStreetMapSplineTools::CleanSplines(SplinesComponent, BuildSettings.RoadMesh, BuildSettings.Landscape->GetWorld());

	TMap< int32, ULandscapeSplineControlPoint* > NodeIndexToControlPointMap;
	const TArray<FStreetMapRoad>& Roads = StreetMapComponent->GetStreetMap()->GetRoads();

	for (const FStreetMapRoad& Road : Roads)
	{
		if (BuildSettings.Type != Road.RoadType)
		{
			continue;
		}

		ULandscapeSplineControlPoint* PreviousPoint = nullptr;
		const int32 NumPoints = Road.RoadPoints.Num();
		for (int32 PointIndex = 0; PointIndex < NumPoints; PointIndex++)
		{
			const FVector2D& PointLocation = Road.RoadPoints[PointIndex];
			const int32 NodeIndex = Road.NodeIndices[PointIndex];

			// Width of the Road depends on type - TODO: look up German rules of standard road geometries
			float RoadWidth = 100.0;
			switch (Road.RoadType)
			{
			case EStreetMapRoadType::Highway:
				RoadWidth = MeshBuildSettings.HighwayThickness; break;
			case EStreetMapRoadType::MajorRoad:
				RoadWidth = MeshBuildSettings.MajorRoadThickness; break;
			case EStreetMapRoadType::Street:
				RoadWidth = MeshBuildSettings.StreetThickness; break;
			case EStreetMapRoadType::Other:
				RoadWidth = 100.0f; break;
			}

			ULandscapeSplineControlPoint* CurrentPoint = nullptr;

			// Try to find existing control point first
			if (NodeIndex != INDEX_NONE)
			{
				ULandscapeSplineControlPoint** CurrentPointPtr = NodeIndexToControlPointMap.Find(NodeIndex);
				if (CurrentPointPtr)
				{
					CurrentPoint = *CurrentPointPtr;
				}
			}

			// create a new control point
			if (CurrentPoint == nullptr)
			{
				const float WorldElevation = FStreetMapSplineTools::GetLandscapeElevation(BuildSettings.Landscape, PointLocation);
				const float ScaledWorldElevation = WorldElevation;
				const FVector2D& ScaledPointLocation = PointLocation;

				CurrentPoint = FStreetMapSplineTools::AddControlPoint(
					SplinesComponent,
					FVector(ScaledPointLocation, ScaledWorldElevation),
					RoadWidth,
					BuildSettings.ZOffset,
					BuildSettings.SideFalloff,
					BuildSettings.EndFalloff,
					BuildSettings.Landscape,
					PreviousPoint);

				if (NodeIndex != INDEX_NONE)
				{
					NodeIndexToControlPointMap.Add(NodeIndex, CurrentPoint);
				}
			}

			if (PreviousPoint)
			{
				ULandscapeSplineSegment* NewSegment = FStreetMapSplineTools::AddSegment(PreviousPoint, CurrentPoint, true, true);

				if (PointIndex == 1)
				{
					FLandscapeSplineMeshEntry MeshEntry;
					MeshEntry.Mesh = BuildSettings.RoadMesh;
					MeshEntry.bScaleToWidth = BuildSettings.ScaleToWidth;
					MeshEntry.ForwardAxis = BuildSettings.ForwardAxis;
					MeshEntry.UpAxis = BuildSettings.UpAxis;
					MeshEntry.Scale = FVector(1.0f);

					NewSegment->LayerName = "soil";
					NewSegment->LDMaxDrawDistance = 100000.0f;
					NewSegment->bRaiseTerrain = true;
					NewSegment->bLowerTerrain = true;
					NewSegment->bPlaceSplineMeshesInStreamingLevels = true;
					NewSegment->CollisionProfileName = UCollisionProfile::NoCollision_ProfileName;
					NewSegment->bCastShadow = true;

					if (NewSegment->SplineMeshes.Num() == 0)
					{
						NewSegment->SplineMeshes.Add(MeshEntry);
					}
				}
			}
			PreviousPoint = CurrentPoint;
		}
	}
}


void BuildRoads(class UStreetMapComponent* StreetMapComponent, const FStreetMapRoadBuildSettings& BuildSettings, const FStreetMapMeshBuildSettings& MeshBuildSettings)
{
	FScopedTransaction Transaction(LOCTEXT("Undo", "Creating Roads"));

	BuildStreetMapRoads(StreetMapComponent, BuildSettings, MeshBuildSettings);
}
