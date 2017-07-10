#pragma once

#include "CoreMinimal.h"

/**
* Creates some additional information based on the given points that form a closed polygon.
* It relies on the existence of the given vertices even after its creation.
*/
struct FPolygon2DView
{
public:

private:

	struct FEdge
	{
		FVector2D Center;
		FVector2D Direction;
		float	  Extent;
	};

	const TArray<FVector2D>&		Vertices;
	TArray<FEdge>					Edges;

public:

	FPolygon2DView(const TArray<FVector2D>& Vertices)
		: Vertices(Vertices)
	{
		Edges.SetNumUninitialized(Vertices.Num());

		const uint32 NumVertices = GetNumVertices();
		const uint32 NumEdges = GetNumEdges();

		uint32 VertexIndex0 = NumVertices - 1;
		for (uint32 EdgeIndex = 0; EdgeIndex < NumEdges; EdgeIndex++)
		{
			const uint32 VertexIndex1 = EdgeIndex;

			const FVector2D& Vertex0 = Vertices[VertexIndex0];
			const FVector2D& Vertex1 = Vertices[VertexIndex1];

			FEdge& Edge = Edges[EdgeIndex];
			Edge.Center = (Vertex0 + Vertex1) * 0.5f;
			(Vertex1 - Vertex0).ToDirectionAndLength(Edge.Direction, Edge.Extent);
			Edge.Extent *= 0.5f;

			VertexIndex0 = VertexIndex1;
		}
	}

	float ComputeSquareDistance(const FVector2D& Point, bool& OutIsInside, FVector2D* OutClosestPoint = nullptr) const
	{
		OutIsInside = false;
		float MinSquareDistance = TNumericLimits<float>::Max();
		FVector2D ClosestPointLocal;
		FVector2D* ClosestPointLocalPtr = OutClosestPoint ? &ClosestPointLocal : NULL;
		uint32 VertexIndex0 = GetNumVertices() - 1;
		for (uint32 EdgeIndex = 0; EdgeIndex < GetNumEdges(); EdgeIndex++)
		{
			const uint32 VertexIndex1 = EdgeIndex;

			float SquareDistance = ComputeSquareDistanceToEdge(Point, VertexIndex0, VertexIndex1, OutIsInside, ClosestPointLocalPtr);
			if (SquareDistance < MinSquareDistance)
			{
				MinSquareDistance = SquareDistance;
				if (OutClosestPoint)
				{
					*OutClosestPoint = ClosestPointLocal;
				}
			}

			VertexIndex0 = VertexIndex1;
		}

		return MinSquareDistance;
	}


	uint32 GetNumEdges() const
	{
		return Edges.Num();
	}

	uint32 GetNumVertices() const
	{
		return Vertices.Num();
	}

private:

	double ComputeSquareDistanceToEdge(const FVector2D& Point, const uint32 VertexIndex0, const uint32 VertexIndex1, bool& CountAsIntersection, FVector2D* OutClosestPoint = nullptr) const
	{
		const uint32 NumVertices = GetNumVertices();
		const FEdge& Edge = Edges[VertexIndex1];

		const FVector2D DirToEdgeCenter = Point - Edge.Center;
		const float SegmentParameter = FVector2D::DotProduct(Edge.Direction, DirToEdgeCenter);
		FVector2D SegmentClosestPoint;
		if (-Edge.Extent < SegmentParameter)
		{
			if (SegmentParameter < Edge.Extent)
			{
				SegmentClosestPoint = Edge.Center + Edge.Direction * SegmentParameter;
			}
			else
			{
				SegmentClosestPoint = Vertices[VertexIndex1];
			}
		}
		else
		{
			SegmentClosestPoint = Vertices[VertexIndex0];
		}

		const FVector2D DirToClosestPoint = Point - SegmentClosestPoint;
		float SquareDistance = FVector2D::DotProduct(DirToClosestPoint, DirToClosestPoint);

		const FVector2D& Vertex0 = Vertices[VertexIndex0];
		const FVector2D& Vertex1 = Vertices[VertexIndex1];

		if ((Vertex0.Y > Point.Y) != (Vertex1.Y > Point.Y))
		{
			CountAsIntersection ^= (Point.X < ((Vertex1.X - Vertex0.X) * (Point.Y - Vertex0.Y) / (Vertex1.Y - Vertex0.Y) + Vertex0.X));
		}

		if (OutClosestPoint)
		{
			*OutClosestPoint = SegmentClosestPoint;
		}

		return SquareDistance;
	}
};
