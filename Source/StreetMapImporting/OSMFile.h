// Copyright 2017 Mike Fricker. All Rights Reserved.
#pragma once

#include "FastXml.h"
#include "GISUtils/SpatialReferenceSystem.h"


/** OpenStreetMap file loader */
class FOSMFile : public IFastXmlCallback
{
	
public:
	
	/** Default constructor for FOSMFile */
	FOSMFile();

	/** Destructor for FOSMFile */
	virtual ~FOSMFile();

	/** Loads the map from an OpenStreetMap XML file.  Note that in the case of the file path containing the XML data, the string must be mutable for us to parse it quickly. */
	bool LoadOpenStreetMapFile( FString& OSMFilePath, const bool bIsFilePathActuallyTextBuffer, class FFeedbackContext* FeedbackContext );


	struct FOSMWayInfo;

	/** Types of ways */
	enum class EOSMWayType
	{
		/** Used for identifying any kind of road, street or path. */
		Highway,

		/** Including light rail, mainline railways, metros, monorails and Trams. */
		Railway,

		/** Used to mark areas as a building. */
		Building,

		/** The leisure tag is for places people go in their spare time (e.g. parks, pitches). */
		Leisure,

		/** Used to describe natural and physical land features (e.g. wood, beach, water). */
		Natural,

		/** Used to describe the primary use of land by humans (e.g. grass, meadow, forest). */
		LandUse,

		/** Currently unrecognized type */
		Other,
	};

	/** Types of a relations we support - there are many, but most are not yet relevant */
	enum class 	EOSMRelationType
	{
		///** Route - like a street network, e.g. highway A5 as a whole with all ways making up the route */
		//Route,

		/** Boundary - marking non renderable lines defining borders of land zones, city borders, etc. - useful to switch rendering modes */
		Boundary,

		/** Multipolygon - has one outer and unrestricted amount of inners - a forest might be modeled like that */
		Multipolygon,

		/** Currently ignored types - Route (can be enabled, but bloats the imported file), TMC, Turn-Restrictions, Election, RouteMaster, Network */
		Other,
	};

	/** Types of a relations member - node, way or another relation */
	enum class EOSMRelationMemberType
	{
		/** Node */
		Node,

		/** Way */
		Way,

		/** Relation */
		Relation,

		/** Unrecognized */
		Other,
	};

	/** Types of a relations role - outer or inner */
	enum class EOSMRelationMemberRole
	{
		/** Outer */
		Outer,

		/** Inner */
		Inner,

		/** Unrecognized */
		Other,
	};


	struct FOSMWayRef
	{
		// Way that we're referencing at this node
		FOSMWayInfo* Way;
			
		// Index of the node in the way's array of nodes
		int32 NodeIndex;
	};

	struct FOSMTag
	{
		FName Key;
		FName Value;
	};

	struct FOSMNodeInfo
	{
		double Latitude;
		double Longitude;
		TArray<FOSMTag> Tags;
		TArray<FOSMWayRef> WayRefs;
	};
		
	struct FOSMWayInfo
	{
		FString Name;
		FString Ref;
		int64 Id;

		TArray<FOSMNodeInfo*> Nodes;
		EOSMWayType WayType;
		/** subtype according to WayType */
		FString Category;

		///
		/// BUILDING
		///
		double Height;
		int32 BuildingLevels;

		///
		/// HIGHWAY
		///
		/** If true, way is only traversable in the order the nodes are listed in the Nodes list */
		uint8 bIsOneWay : 1;
	};

	struct FOSMRelationMember
	{
		EOSMRelationMemberType Type;
		EOSMRelationMemberRole Role;
		int64 Ref;
	};

	struct FOSMRelation
	{
		EOSMRelationType Type;
		TArray<FOSMRelationMember*> Members;
		TArray<FOSMTag> Tags;
	};

	// Minimum latitude/longitude bounds
	double MinLatitude = MAX_dbl;
	double MinLongitude = MAX_dbl;
	double MaxLatitude = -MAX_dbl;
	double MaxLongitude = -MAX_dbl;

	FSpatialReferenceSystem SpatialReferenceSystem;

	// All ways we've parsed
	TArray<FOSMWayInfo*> Ways;

	// All relations we've parsed
	TArray<FOSMRelation*> Relations;
		
	// Maps node IDs to info about each node
	TMap<int64, FOSMNodeInfo*> NodeMap;

	// Maps node IDs to info about each node
	TMap<int64, FOSMWayInfo*> WayMap;

protected:

	// IFastXmlCallback overrides
	virtual bool ProcessXmlDeclaration( const TCHAR* ElementData, int32 XmlFileLineNumber ) override;
	virtual bool ProcessComment( const TCHAR* Comment ) override;
	virtual bool ProcessElement( const TCHAR* ElementName, const TCHAR* ElementData, int32 XmlFileLineNumber ) override;
	virtual bool ProcessAttribute( const TCHAR* AttributeName, const TCHAR* AttributeValue ) override;
	virtual bool ProcessClose( const TCHAR* Element ) override;

	
protected:
	
	enum class ParsingState
	{
		Root,
		Bounds,
		Node,
		Node_Tag,
		Way,
		Way_NodeRef,
		Way_Tag,
		Relation,
		Relation_Member,
		Relation_Tag,
	};
		
	// Current state of parser
	ParsingState ParsingState;
		
	// ID of node that is currently being parsed
	int64 CurrentNodeID;

	// ID of way that is currently being parsed
	int64 CurrentWayID;

	// ID of relation that is currently being parsed
	int64 CurrentRelationID;

	// Node that is currently being parsed
	FOSMNodeInfo* CurrentNodeInfo;
		
	// Way that is currently being parsed
	FOSMWayInfo* CurrentWayInfo;

	// Relation that is currently being parsed
	FOSMRelation* CurrentRelation;

	// Relation meber that is currently being parsed
	FOSMRelationMember* CurrentRelationMember;

	// Current way's tag key string
	const TCHAR* CurrentWayTagKey;

	// Current nodes's tag key string
	const TCHAR* CurrentNodeTagKey;

	// Current relation's tag key string
	const TCHAR* CurrentRelationTagKey;
};


