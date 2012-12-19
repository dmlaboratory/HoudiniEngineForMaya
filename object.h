#ifndef OBJECT_H
#define OBJECT_H

#include <maya/MStatus.h>
#include <maya/MFloatArray.h>
#include <maya/MFloatPointArray.h>
#include <maya/MVectorArray.h>
#include <maya/MStringArray.h>
#include <maya/MIntArray.h>
#include <maya/MObject.h>
#include <maya/MPlug.h>
#include <maya/MDataBlock.h>

#include <HAPI.h>

class Asset;

class Object {


    public:
        enum ObjectType {
            OBJECT_TYPE_GEOMETRY,
            OBJECT_TYPE_INSTANCER
        };

        // static creator
        static Object* createObject(int assetId, int objectId, Asset* objControl);

        Object();
        Object(int assetId, int objectId);
        virtual ~Object();

        virtual void init();

        virtual int getId();
        virtual MString getName();

        //virtual MStatus compute(const MPlug& plug, MDataBlock& data);
        virtual MStatus compute(MDataHandle& handle) = 0;
        virtual MStatus setClean(MPlug& plug, MDataBlock& data) = 0;
        virtual ObjectType type() = 0;

        // Utility
        virtual void printAttributes(HAPI_AttributeOwner owner);

    public:
        Asset* objectControl;
        bool isInstanced;

    protected:
        virtual void update();


    protected:
        HAPI_ObjectInfo objectInfo;
        HAPI_GeoInfo geoInfo;
        int assetId;
        int objectId;

        bool neverBuilt;
};

#endif