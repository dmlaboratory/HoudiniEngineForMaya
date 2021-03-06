#include "InputParticle.h"

#include <maya/MFnAttribute.h>
#include <maya/MFnParticleSystem.h>
#include <maya/MDoubleArray.h>
#include <maya/MGlobal.h>
#include <maya/MMatrix.h>
#include <maya/MPlug.h>
#include <maya/MPlugArray.h>
#include <maya/MStringArray.h>
#include <maya/MVectorArray.h>

#include "hapiutil.h"
#include "util.h"

InputParticle::InputParticle(int assetId, int inputIdx) :
    Input(assetId, inputIdx),
    myInputAssetId(0)
{
    Util::PythonInterpreterLock pythonInterpreterLock;

    CHECK_HAPI(HAPI_CreateInputAsset(
                Util::theHAPISession.get(),
                &myInputAssetId,
                NULL
                ));

    if(!Util::statusCheckLoop())
    {
        DISPLAY_ERROR(MString("Unexpected error when creating input asset."));
    }

    myInputObjectId = 0;
    myInputGeoId = 0;

    CHECK_HAPI(HAPI_ConnectAssetGeometry(
                Util::theHAPISession.get(),
                myInputAssetId, myInputObjectId,
                myAssetId, myInputIdx
                ));
}

InputParticle::~InputParticle()
{
    CHECK_HAPI(HAPI_DestroyAsset(
                Util::theHAPISession.get(),
                myInputAssetId
                ));
}

Input::AssetInputType
InputParticle::assetInputType() const
{
    return Input::AssetInputType_Particle;
}

void
InputParticle::setInputTransform(MDataHandle &dataHandle)
{
    MMatrix transformMatrix = dataHandle.asMatrix();

    float matrix[16];
    transformMatrix.get(reinterpret_cast<float(*)[4]>(matrix));

    HAPI_TransformEuler transformEuler;
    HAPI_ConvertMatrixToEuler(
            Util::theHAPISession.get(),
            matrix,
            HAPI_SRT,
            HAPI_XYZ,
            &transformEuler
            );
    HAPI_SetObjectTransform(
            Util::theHAPISession.get(),
            myInputAssetId,
            myInputObjectId,
            &transformEuler
            );
}

void
InputParticle::setAttributePointData(
        const char* attributeName,
        HAPI_StorageType storage,
        int count,
        int tupleSize,
        void* data
        )
{
    HAPI_AttributeInfo attributeInfo;
    attributeInfo.exists = true;
    attributeInfo.owner = HAPI_ATTROWNER_POINT;
    attributeInfo.storage = storage;
    attributeInfo.count = count;
    attributeInfo.tupleSize = tupleSize;

    HAPI_AddAttribute(
            Util::theHAPISession.get(),
            myInputAssetId, myInputObjectId, myInputGeoId,
            attributeName,
            &attributeInfo
            );

    switch(storage)
    {
        case HAPI_STORAGETYPE_FLOAT:
            HAPI_SetAttributeFloatData(
                    Util::theHAPISession.get(),
                    myInputAssetId, myInputObjectId, myInputGeoId,
                    attributeName,
                    &attributeInfo,
                    static_cast<float*>(data),
                    0, count
                    );
            break;
        case HAPI_STORAGETYPE_INT:
            HAPI_SetAttributeIntData(
                    Util::theHAPISession.get(),
                    myInputAssetId, myInputObjectId, myInputGeoId,
                    attributeName,
                    &attributeInfo,
                    static_cast<int*>(data),
                    0, count
                    );
            break;
        default:
            break;
    }
}

void
InputParticle::setInputGeo(
        MDataBlock &dataBlock,
        const MPlug &plug
        )
{
    MStatus status;

    // get particle node
    MObject particleObj;
    {
        MPlugArray plugArray;
        plug.connectedTo(plugArray, true, false, &status);
        CHECK_MSTATUS(status);

        if(plugArray.length() == 1)
        {
            particleObj = plugArray[0].node();
        }
    }

    if(particleObj.isNull())
    {
        return;
    }

    MFnParticleSystem particleFn(particleObj);

    // get original particle node
    MObject originalParticleObj;
    // status parameter is needed due to a bug in Maya API
    if(particleFn.isDeformedParticleShape(&status))
    {
        originalParticleObj = particleFn.originalParticleShape(&status);
    }
    else
    {
        originalParticleObj = particleObj;
    }

    MFnParticleSystem originalParticleFn(originalParticleObj);

    // set up part info
    HAPI_PartInfo partInfo;
    HAPI_PartInfo_Init(&partInfo);
    partInfo.id = 0;
    partInfo.faceCount = 0;
    partInfo.vertexCount = 0;
    partInfo.pointCount = particleFn.count();

    HAPI_SetPartInfo(
            Util::theHAPISession.get(),
            myInputAssetId, myInputObjectId, myInputGeoId,
            &partInfo
            );

    // set per-particle attributes
    {
        // id
        {
            MIntArray ids;
            // Must get the IDs from the original particle node. Maya will
            // crash if we try to get the IDs from the deformed particle node.
            originalParticleFn.particleIds(ids);

            CHECK_HAPI(hapiSetPointAttribute(
                    myInputAssetId, myInputObjectId, myInputGeoId,
                    1,
                    "id",
                    ids
                    ));
        }

        // vector attributes
        {
            MVectorArray vectorArray;

            // query the original particle for names of the per-particle
            // attributes
            MString getAttributesCommand = "particle -q -perParticleVector ";
            getAttributesCommand += originalParticleFn.fullPathName();

            // get the per-particle attribute names
            MStringArray attributeNames;
            MGlobal::executeCommand(getAttributesCommand, attributeNames);

            for(unsigned int i = 0; i < attributeNames.length(); i++)
            {
                const MString attributeName = attributeNames[i];

                MObject attributeObj = originalParticleFn.attribute(attributeName);
                if(attributeObj.isNull())
                {
                    continue;
                }

                // mimics "listAttr -v -w" from AEokayAttr
                MFnAttribute attributeFn(attributeObj);
                if(!(!attributeFn.isHidden() && attributeFn.isWritable())
                        && (attributeName != "age"
                            && attributeName != "finalLifespanPP"))
                {
                    continue;
                }

                // get the per-particle data
                if(attributeName == "position")
                {
                    // Need to use position() so that we get the right
                    // positions in the case of deformed particles.
                    particleFn.position(vectorArray);
                }
                else
                {
                    // Maya will automatically use the original
                    // particle node in the case of deformed particles.
                    particleFn.getPerParticleAttribute(
                            attributeName,
                            vectorArray
                            );
                }

                // When particle node is initially loaded from a scene file, if
                // the attribute is driven by expressions, then
                // MFnParticleSystem doesn't initially seem to have data.
                if(partInfo.pointCount != (int) vectorArray.length())
                {
                    vectorArray.setLength(partInfo.pointCount);
                }

                // map the attribute name
                const char* mappedAttributeName = attributeName.asChar();
                if(strcmp(mappedAttributeName, "position") == 0)
                {
                    mappedAttributeName = "P";
                }
                else if(strcmp(mappedAttributeName, "velocity") == 0)
                {
                    mappedAttributeName = "v";
                }
                else if(strcmp(mappedAttributeName, "acceleration") == 0)
                {
                    mappedAttributeName = "force";
                }
                else if(strcmp(mappedAttributeName, "rgbPP") == 0)
                {
                    mappedAttributeName = "Cd";
                }

                CHECK_HAPI(hapiSetPointAttribute(
                            myInputAssetId, myInputObjectId, myInputGeoId,
                            3,
                            mappedAttributeName,
                            Util::reshapeArray<
                                3,
                                std::vector<double>
                                >(vectorArray)
                            ));
            }
        }

        // double attributes
        {
            MDoubleArray doubleArray;

            // query the original particle for names of the per-particle
            // attributes
            MString getAttributesCommand = "particle -q -perParticleDouble ";
            getAttributesCommand += originalParticleFn.fullPathName();

            // get the per-particle attribute names
            MStringArray attributeNames;
            MGlobal::executeCommand(getAttributesCommand, attributeNames);

            // explicitly include some special per-particle attributes that
            // aren't returned by the MEL command
            attributeNames.append("age");

            for(unsigned int i = 0; i < attributeNames.length(); i++)
            {
                const MString attributeName = attributeNames[i];

                MObject attributeObj = originalParticleFn.attribute(attributeName);
                if(attributeObj.isNull())
                {
                    continue;
                }

                // mimics "listAttr -v -w" from AEokayAttr
                MFnAttribute attributeFn(attributeObj);
                if(!(!attributeFn.isHidden() && attributeFn.isWritable())
                        && (attributeName != "age"
                            && attributeName != "finalLifespanPP"))
                {
                    continue;
                }

                // get the per-particle data
                // Maya will automatically use the original
                // particle node in the case of deformed particles.
                particleFn.getPerParticleAttribute(
                        attributeName,
                        doubleArray
                        );

                // When particle node is initially loaded from a scene file, if
                // the attribute is driven by expressions, then
                // MFnParticleSystem doesn't initially seem to have data.
                if(partInfo.pointCount != (int) doubleArray.length())
                {
                    doubleArray.setLength(partInfo.pointCount);
                }

                // map the parameter name
                const char* mappedAttributeName = attributeName.asChar();
                if(strcmp(mappedAttributeName, "opacityPP") == 0)
                {
                    mappedAttributeName = "Alpha";
                }
                else if(strcmp(mappedAttributeName, "radiusPP") == 0)
                {
                    mappedAttributeName = "pscale";
                }
                else if(strcmp(mappedAttributeName, "finalLifespanPP") == 0)
                {
                    mappedAttributeName = "life";
                }

                CHECK_HAPI(hapiSetPointAttribute(
                            myInputAssetId, myInputObjectId, myInputGeoId,
                            1,
                            mappedAttributeName,
                            doubleArray
                            ));
            }
        }
    }

    Input::setInputPlugMetaData(
            plug,
            myInputAssetId, myInputObjectId, myInputGeoId
            );

    // Commit it
    HAPI_CommitGeo(
            Util::theHAPISession.get(),
            myInputAssetId, myInputObjectId, myInputGeoId
            );
}
