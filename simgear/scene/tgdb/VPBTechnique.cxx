// VPBTechnique.cxx -- VirtualPlanetBuilder Effects technique
//
// Copyright (C) 2020 Stuart Buchanan
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include <cmath>
#include <tuple>

#include <osgTerrain/TerrainTile>
#include <osgTerrain/Terrain>

#include <osgUtil/LineSegmentIntersector>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/MeshOptimizers>
#include <osgUtil/Tessellator>

#include <osgDB/FileUtils>
#include <osgDB/ReadFile>

#include <osg/io_utils>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include <osg/Texture1D>
#include <osg/Program>
#include <osg/Math>
#include <osg/Timer>

#include <simgear/debug/logstream.hxx>
#include <simgear/math/sg_random.hxx>
#include <simgear/math/SGMath.hxx>
#include <simgear/scene/material/Effect.hxx>
#include <simgear/scene/material/EffectGeode.hxx>
#include <simgear/scene/model/model.hxx>
#include <simgear/scene/tgdb/VPBElevationSlice.hxx>
#include <simgear/scene/tgdb/VPBTileBounds.hxx>
#include <simgear/scene/util/SGNodeMasks.hxx>
#include <simgear/scene/util/SGReaderWriterOptions.hxx>
#include <simgear/scene/util/SGSceneFeatures.hxx>

#include "VPBTechnique.hxx"
#include "VPBMaterialHandler.hxx"

using namespace osgTerrain;
using namespace simgear;

VPBTechnique::VPBTechnique()
{
    setFilterBias(0);
    setFilterWidth(0.1);
    setFilterMatrixAs(GAUSSIAN);
    _randomObjectsConstraintGroup = new osg::Group();
}

VPBTechnique::VPBTechnique(const SGReaderWriterOptions* options, const string fileName):
    _fileName(fileName)
{
    setFilterBias(0);
    setFilterWidth(0.1);
    setFilterMatrixAs(GAUSSIAN);
    setOptions(options);
    _randomObjectsConstraintGroup = new osg::Group();
}

VPBTechnique::VPBTechnique(const VPBTechnique& gt,const osg::CopyOp& copyop):
    TerrainTechnique(gt,copyop),
    _fileName(gt._fileName)
{
    setFilterBias(gt._filterBias);
    setFilterWidth(gt._filterWidth);
    setFilterMatrix(gt._filterMatrix);
    setOptions(gt._options);
    _randomObjectsConstraintGroup = new osg::Group();
}

VPBTechnique::~VPBTechnique()
{
}

void VPBTechnique::setFilterBias(float filterBias)
{
    _filterBias = filterBias;
    if (!_filterBiasUniform) _filterBiasUniform = new osg::Uniform("filterBias",_filterBias);
    else _filterBiasUniform->set(filterBias);
}

void VPBTechnique::setFilterWidth(float filterWidth)
{
    _filterWidth = filterWidth;
    if (!_filterWidthUniform) _filterWidthUniform = new osg::Uniform("filterWidth",_filterWidth);
    else _filterWidthUniform->set(filterWidth);
}

void VPBTechnique::setFilterMatrix(const osg::Matrix3& matrix)
{
    _filterMatrix = matrix;
    if (!_filterMatrixUniform) _filterMatrixUniform = new osg::Uniform("filterMatrix",_filterMatrix);
    else _filterMatrixUniform->set(_filterMatrix);
}

void VPBTechnique::setOptions(const SGReaderWriterOptions* options)
{
    _options = simgear::SGReaderWriterOptions::copyOrCreate(options);
    _options->setLoadOriginHint(simgear::SGReaderWriterOptions::LoadOriginHint::ORIGIN_EFFECTS);
    _options->setInstantiateMaterialEffects(true);
}

void VPBTechnique::setFilterMatrixAs(FilterType filterType)
{
    switch(filterType)
    {
        case(SMOOTH):
            setFilterMatrix(osg::Matrix3(0.0, 0.5/2.5, 0.0,
                                         0.5/2.5, 0.5/2.5, 0.5/2.5,
                                         0.0, 0.5/2.5, 0.0));
            break;
        case(GAUSSIAN):
            setFilterMatrix(osg::Matrix3(0.0, 1.0/8.0, 0.0,
                                         1.0/8.0, 4.0/8.0, 1.0/8.0,
                                         0.0, 1.0/8.0, 0.0));
            break;
        case(SHARPEN):
            setFilterMatrix(osg::Matrix3(0.0, -1.0, 0.0,
                                         -1.0, 5.0, -1.0,
                                         0.0, -1.0, 0.0));
            break;

    };
}

void VPBTechnique::init(int dirtyMask, bool assumeMultiThreaded)
{
    if (!_terrainTile) return;
    if (dirtyMask==0) return;

    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_writeBufferMutex);

    osg::ref_ptr<TerrainTile> tile = _terrainTile;

    osgTerrain::TileID tileID = tile->getTileID();
    SG_LOG(SG_TERRAIN, SG_DEBUG, "Init of tile " << tileID.x << "," << tileID.y << " level " << tileID.level << " " << dirtyMask);

    osg::ref_ptr<BufferData> buffer = new BufferData;

    Locator* masterLocator = computeMasterLocator();

    osg::Vec3d centerModel = computeCenterModel(*buffer, masterLocator);

    if ((dirtyMask & TerrainTile::IMAGERY_DIRTY)==0)
    {
        generateGeometry(*buffer, masterLocator, centerModel);

        osg::ref_ptr<BufferData> read_buffer = _currentBufferData;

        osg::StateSet* stateset = read_buffer->_landGeode->getStateSet();
        if (stateset)
        {
            buffer->_landGeode->setStateSet(stateset);
        }
        else
        {
            applyColorLayers(*buffer, masterLocator);
            applyLineFeatures(*buffer, masterLocator);
            applyAreaFeatures(*buffer, masterLocator);
            applyCoastline(*buffer, masterLocator);
            applyMaterials(*buffer, masterLocator);
        }
    }
    else
    {
        generateGeometry(*buffer, masterLocator, centerModel);
        
        applyColorLayers(*buffer, masterLocator);
        applyLineFeatures(*buffer, masterLocator);
        applyAreaFeatures(*buffer, masterLocator);
        applyCoastline(*buffer, masterLocator);
        applyMaterials(*buffer, masterLocator);
    }

    if (buffer->_transform.valid()) buffer->_transform->setThreadSafeRefUnref(true);

    if (!_currentBufferData || !assumeMultiThreaded)
    {
        // no currentBufferData so we must be the first init to be applied
        _currentBufferData = buffer;
    }
    else
    {
        // there is already an active _currentBufferData so we'll request that this gets swapped on next frame.
        _newBufferData = buffer;
        if (_terrainTile->getTerrain()) _terrainTile->getTerrain()->updateTerrainTileOnNextFrame(_terrainTile);
    }

    _terrainTile->setDirtyMask(0);
}

Locator* VPBTechnique::computeMasterLocator()
{
    osgTerrain::Layer* elevationLayer = _terrainTile->getElevationLayer();
    osgTerrain::Layer* colorLayer = _terrainTile->getColorLayer(0);

    Locator* elevationLocator = elevationLayer ? elevationLayer->getLocator() : 0;
    Locator* colorLocator = colorLayer ? colorLayer->getLocator() : 0;

    Locator* masterLocator = elevationLocator ? elevationLocator : colorLocator;
    if (!masterLocator)
    {
        OSG_NOTICE<<"Problem, no locator found in any of the terrain layers"<<std::endl;
        return 0;
    }

    return masterLocator;
}

osg::Vec3d VPBTechnique::computeCenter(BufferData& buffer, Locator* masterLocator)
{
    if (!masterLocator) return osg::Vec3d(0.0,0.0,0.0);

    osgTerrain::Layer* elevationLayer = _terrainTile->getElevationLayer();
    osgTerrain::Layer* colorLayer = _terrainTile->getColorLayer(0);

    Locator* elevationLocator = elevationLayer ? elevationLayer->getLocator() : 0;
    Locator* colorLocator = colorLayer ? colorLayer->getLocator() : 0;

    if (!elevationLocator) elevationLocator = masterLocator;
    if (!colorLocator) colorLocator = masterLocator;

    osg::Vec3d bottomLeftNDC(DBL_MAX, DBL_MAX, 0.0);
    osg::Vec3d topRightNDC(-DBL_MAX, -DBL_MAX, 0.0);

    if (elevationLayer)
    {
        if (elevationLocator!= masterLocator)
        {
            masterLocator->computeLocalBounds(*elevationLocator, bottomLeftNDC, topRightNDC);
        }
        else
        {
            bottomLeftNDC.x() = osg::minimum(bottomLeftNDC.x(), 0.0);
            bottomLeftNDC.y() = osg::minimum(bottomLeftNDC.y(), 0.0);
            topRightNDC.x() = osg::maximum(topRightNDC.x(), 1.0);
            topRightNDC.y() = osg::maximum(topRightNDC.y(), 1.0);
        }
    }

    if (colorLayer)
    {
        if (colorLocator!= masterLocator)
        {
            masterLocator->computeLocalBounds(*colorLocator, bottomLeftNDC, topRightNDC);
        }
        else
        {
            bottomLeftNDC.x() = osg::minimum(bottomLeftNDC.x(), 0.0);
            bottomLeftNDC.y() = osg::minimum(bottomLeftNDC.y(), 0.0);
            topRightNDC.x() = osg::maximum(topRightNDC.x(), 1.0);
            topRightNDC.y() = osg::maximum(topRightNDC.y(), 1.0);
        }
    }

    OSG_INFO<<"bottomLeftNDC = "<<bottomLeftNDC<<std::endl;
    OSG_INFO<<"topRightNDC = "<<topRightNDC<<std::endl;

    osg::Vec3d centerNDC = (bottomLeftNDC + topRightNDC)*0.5;
    return centerNDC;
}

osg::Vec3d VPBTechnique::computeCenterModel(BufferData& buffer, Locator* masterLocator)
{
    osg::Vec3d centerNDC = computeCenter(buffer, masterLocator);
    osg::Vec3d centerModel = centerNDC;
    masterLocator->convertLocalToModel(centerNDC, centerModel);

    buffer._transform = new osg::MatrixTransform;
    buffer._transform->setMatrix(osg::Matrix::translate(centerModel));

    return centerModel;
}

const SGGeod VPBTechnique::computeCenterGeod(BufferData& buffer, Locator* masterLocator)
{
    const osg::Vec3d world = buffer._transform->getMatrix().getTrans();
    return SGGeod::fromCart(toSG(world));
}

class VertexNormalGenerator
{
    public:

        typedef std::vector<int> Indices;
        typedef std::pair< osg::ref_ptr<osg::Vec2Array>, Locator* > TexCoordLocatorPair;
        typedef std::map< Layer*, TexCoordLocatorPair > LayerToTexCoordMap;

        VertexNormalGenerator(Locator* masterLocator, const osg::Vec3d& centerModel, int numRows, int numColmns, float scaleHeight, float vtx_gap, bool createSkirt);

        void populateCenter(osgTerrain::Layer* elevationLayer, osg::Vec2Array* texcoords);
        void populateLeftBoundary(osgTerrain::Layer* elevationLayer);
        void populateRightBoundary(osgTerrain::Layer* elevationLayer);
        void populateAboveBoundary(osgTerrain::Layer* elevationLayer);
        void populateBelowBoundary(osgTerrain::Layer* elevationLayer);

        void computeNormals();

        unsigned int capacity() const { return _vertices->capacity(); }

        inline void setVertex(int c, int r, const osg::Vec3& v, const osg::Vec3& n)
        {
            int& i = index(c,r);
            if (i==0)
            {
                if (r<0 || r>=_numRows || c<0 || c>=_numColumns)
                {
                    i = -(1+static_cast<int>(_boundaryVertices->size()));
                    _boundaryVertices->push_back(v);
                    // OSG_NOTICE<<"setVertex("<<c<<", "<<r<<", ["<<v<<"], ["<<n<<"]), i="<<i<<" _boundaryVertices["<<-i-1<<"]="<<(*_boundaryVertices)[-i-1]<<"]"<<std::endl;
                }
                else
                {
                    i = _vertices->size() + 1;
                    _vertices->push_back(v);
                    _normals->push_back(n);
                    // OSG_NOTICE<<"setVertex("<<c<<", "<<r<<", ["<<v<<"], ["<<n<<"]), i="<<i<<" _vertices["<<i-1<<"]="<<(*_vertices)[i-1]<<"]"<<std::endl;
                }
            }
            else if (i<0)
            {
                (*_boundaryVertices)[-i-1] = v;
                // OSG_NOTICE<<"setVertex("<<c<<", "<<r<<", ["<<v<<"], ["<<n<<"] _boundaryVertices["<<-i-1<<"]="<<(*_boundaryVertices)[-i-1]<<"]"<<std::endl;
            }
            else
            {
                // OSG_NOTICE<<"Overwriting setVertex("<<c<<", "<<r<<", ["<<v<<"], ["<<n<<"]"<<std::endl;
                // OSG_NOTICE<<"     previous values ( vertex ["<<(*_vertices)[i-1]<<"], normal (*_normals)[i-1] ["<<n<<"]"<<std::endl;
                // (*_vertices)[i-1] = v;

                // average the vertex positions
                (*_vertices)[i-1] = ((*_vertices)[i-1] + v)*0.5f;

                (*_normals)[i-1] = n;
            }
        }

        inline int& index(int c, int r) { return _indices[(r+1)*(_numColumns+2)+c+1]; }

        inline int index(int c, int r) const { return _indices[(r+1)*(_numColumns+2)+c+1]; }

        inline int vertex_index(int c, int r) const { int i = _indices[(r+1)*(_numColumns+2)+c+1]; return i-1; }

        inline bool vertex(int c, int r, osg::Vec3& v) const
        {
            int i = index(c,r);
            if (i==0) return false;
            if (i<0) v = (*_boundaryVertices)[-i-1];
            else v = (*_vertices)[i-1];
            return true;
        }

        inline bool computeNormal(int c, int r, osg::Vec3& n) const
        {
#if 1
            return computeNormalWithNoDiagonals(c,r,n);
#else
            return computeNormalWithDiagonals(c,r,n);
#endif
        }

        inline bool computeNormalWithNoDiagonals(int c, int r, osg::Vec3& n) const
        {
            osg::Vec3 center;
            bool center_valid  = vertex(c, r,  center);
            if (!center_valid) return false;

            osg::Vec3 left, right, top,  bottom;
            bool left_valid  = vertex(c-1, r,  left);
            bool right_valid = vertex(c+1, r,   right);
            bool bottom_valid = vertex(c,   r-1, bottom);
            bool top_valid = vertex(c,   r+1, top);

            osg::Vec3 dx(0.0f,0.0f,0.0f);
            osg::Vec3 dy(0.0f,0.0f,0.0f);
            osg::Vec3 zero(0.0f,0.0f,0.0f);
            if (left_valid)
            {
                dx += center-left;
            }
            if (right_valid)
            {
                dx += right-center;
            }
            if (bottom_valid)
            {
                dy += center-bottom;
            }
            if (top_valid)
            {
                dy += top-center;
            }

            if (dx==zero || dy==zero) return false;

            n = dx ^ dy;
            return n.normalize() != 0.0f;
        }

        inline bool computeNormalWithDiagonals(int c, int r, osg::Vec3& n) const
        {
            osg::Vec3 center;
            bool center_valid  = vertex(c, r,  center);
            if (!center_valid) return false;

            osg::Vec3 top_left, top_right, bottom_left, bottom_right;
            bool top_left_valid  = vertex(c-1, r+1,  top_left);
            bool top_right_valid  = vertex(c+1, r+1,  top_right);
            bool bottom_left_valid  = vertex(c-1, r-1,  bottom_left);
            bool bottom_right_valid  = vertex(c+1, r-1,  bottom_right);

            osg::Vec3 left, right, top,  bottom;
            bool left_valid  = vertex(c-1, r,  left);
            bool right_valid = vertex(c+1, r,   right);
            bool bottom_valid = vertex(c,   r-1, bottom);
            bool top_valid = vertex(c,   r+1, top);

            osg::Vec3 dx(0.0f,0.0f,0.0f);
            osg::Vec3 dy(0.0f,0.0f,0.0f);
            osg::Vec3 zero(0.0f,0.0f,0.0f);
            const float ratio = 0.5f;
            if (left_valid)
            {
                dx = center-left;
                if (top_left_valid) dy += (top_left-left)*ratio;
                if (bottom_left_valid) dy += (left-bottom_left)*ratio;
            }
            if (right_valid)
            {
                dx = right-center;
                if (top_right_valid) dy += (top_right-right)*ratio;
                if (bottom_right_valid) dy += (right-bottom_right)*ratio;
            }
            if (bottom_valid)
            {
                dy += center-bottom;
                if (bottom_left_valid) dx += (bottom-bottom_left)*ratio;
                if (bottom_right_valid) dx += (bottom_right-bottom)*ratio;
            }
            if (top_valid)
            {
                dy += top-center;
                if (top_left_valid) dx += (top-top_left)*ratio;
                if (top_right_valid) dx += (top_right-top)*ratio;
            }

            if (dx==zero || dy==zero) return false;

            n = dx ^ dy;
            return n.normalize() != 0.0f;
        }

        Locator*                        _masterLocator;
        const osg::Vec3d                _centerModel;
        int                             _numRows;
        int                             _numColumns;
        float                           _scaleHeight;
        float                           _constraint_vtx_gap;

        Indices                         _indices;

        osg::ref_ptr<osg::Vec3Array>    _vertices;
        osg::ref_ptr<osg::Vec3Array>    _normals;
        osg::ref_ptr<osg::FloatArray>   _elevations;

        osg::ref_ptr<osg::Vec3Array>    _boundaryVertices;

};

VertexNormalGenerator::VertexNormalGenerator(Locator* masterLocator, const osg::Vec3d& centerModel, int numRows, int numColumns, float scaleHeight, float vtx_gap, bool createSkirt):
    _masterLocator(masterLocator),
    _centerModel(centerModel),
    _numRows(numRows),
    _numColumns(numColumns),
    _scaleHeight(scaleHeight),
    _constraint_vtx_gap(vtx_gap)
{
    int numVerticesInBody = numColumns*numRows;
    int numVerticesInSkirt = createSkirt ? numColumns*2 + numRows*2 - 4 : 0;
    int numVertices = numVerticesInBody+numVerticesInSkirt;

    _indices.resize((_numRows+2)*(_numColumns+2),0);

    _vertices = new osg::Vec3Array;
    _vertices->reserve(numVertices);

    _normals = new osg::Vec3Array;
    _normals->reserve(numVertices);

    _elevations = new osg::FloatArray;
    _elevations->reserve(numVertices);

    _boundaryVertices = new osg::Vec3Array;
    _boundaryVertices->reserve(_numRows*2 + _numColumns*2 + 4);
}

void VertexNormalGenerator::populateCenter(osgTerrain::Layer* elevationLayer, osg::Vec2Array* texcoords)
{
    // OSG_NOTICE<<std::endl<<"VertexNormalGenerator::populateCenter("<<elevationLayer<<")"<<std::endl;

    bool sampled = elevationLayer &&
                   ( (elevationLayer->getNumRows()!=static_cast<unsigned int>(_numRows)) ||
                     (elevationLayer->getNumColumns()!=static_cast<unsigned int>(_numColumns)) );

    for(int j=0; j<_numRows; ++j)
    {
        for(int i=0; i<_numColumns; ++i)
        {
            osg::Vec3d ndc( ((double)i)/(double)(_numColumns-1), ((double)j)/(double)(_numRows-1), 0.0);

            bool validValue = true;
            if (elevationLayer)
            {
                float value = 0.0f;
                if (sampled) validValue = elevationLayer->getInterpolatedValidValue(ndc.x(), ndc.y(), value);
                else validValue = elevationLayer->getValidValue(i,j,value);
                ndc.z() = value*_scaleHeight;
            }

            if (validValue)
            {
                osg::Vec3d model;
                osg::Vec3d origin;
                _masterLocator->convertLocalToModel(osg::Vec3d(ndc.x(), ndc.y(), -1000), origin);
                _masterLocator->convertLocalToModel(ndc, model);

                model = VPBTechnique::checkAgainstElevationConstraints(origin, model, _constraint_vtx_gap);

                texcoords->push_back(osg::Vec2(ndc.x(), ndc.y()));

                if (_elevations.valid())
                {
                    (*_elevations).push_back(ndc.z());
                }

                // compute the local normal
                osg::Vec3d ndc_one = ndc; ndc_one.z() += 1.0;
                osg::Vec3d model_one;
                _masterLocator->convertLocalToModel(ndc_one, model_one);
                model_one = model_one - model;
                model_one.normalize();

                setVertex(i, j, osg::Vec3(model-_centerModel), model_one);
            } else {
                SG_LOG(SG_TERRAIN, SG_ALERT, "Invalid elevation value found");
            }
        }
    }
}

void VertexNormalGenerator::populateLeftBoundary(osgTerrain::Layer* elevationLayer)
{
    // OSG_NOTICE<<"   VertexNormalGenerator::populateLeftBoundary("<<elevationLayer<<")"<<std::endl;

    if (!elevationLayer) return;

    bool sampled = elevationLayer &&
                   ( (elevationLayer->getNumRows()!=static_cast<unsigned int>(_numRows)) ||
                     (elevationLayer->getNumColumns()!=static_cast<unsigned int>(_numColumns)) );

    for(int j=0; j<_numRows; ++j)
    {
        for(int i=-1; i<=0; ++i)
        {
            osg::Vec3d ndc( ((double)i)/(double)(_numColumns-1), ((double)j)/(double)(_numRows-1), 0.0);
            osg::Vec3d left_ndc( 1.0+ndc.x(), ndc.y(), 0.0);

            bool validValue = true;
            if (elevationLayer)
            {
                float value = 0.0f;
                if (sampled) validValue = elevationLayer->getInterpolatedValidValue(left_ndc.x(), left_ndc.y(), value);
                else validValue = elevationLayer->getValidValue((_numColumns-1)+i,j,value);
                ndc.z() = value*_scaleHeight;

                ndc.z() += 0.f;
            }
            if (validValue)
            {
                osg::Vec3d model;
                _masterLocator->convertLocalToModel(ndc, model);

                // compute the local normal
                osg::Vec3d ndc_one = ndc; ndc_one.z() += 1.0;
                osg::Vec3d model_one;
                _masterLocator->convertLocalToModel(ndc_one, model_one);
                model_one = model_one - model;
                model_one.normalize();

                setVertex(i, j, osg::Vec3(model-_centerModel), model_one);
                // OSG_NOTICE<<"       setVertex("<<i<<", "<<j<<"..)"<<std::endl;
            }
        }
    }
}

void VertexNormalGenerator::populateRightBoundary(osgTerrain::Layer* elevationLayer)
{
    // OSG_NOTICE<<"   VertexNormalGenerator::populateRightBoundary("<<elevationLayer<<")"<<std::endl;

    if (!elevationLayer) return;

    bool sampled = elevationLayer &&
                   ( (elevationLayer->getNumRows()!=static_cast<unsigned int>(_numRows)) ||
                     (elevationLayer->getNumColumns()!=static_cast<unsigned int>(_numColumns)) );

    for(int j=0; j<_numRows; ++j)
    {
        for(int i=_numColumns-1; i<_numColumns+1; ++i)
        {
            osg::Vec3d ndc( ((double)i)/(double)(_numColumns-1), ((double)j)/(double)(_numRows-1), 0.0);
            osg::Vec3d right_ndc(ndc.x()-1.0, ndc.y(), 0.0);

            bool validValue = true;
            if (elevationLayer)
            {
                float value = 0.0f;
                if (sampled) validValue = elevationLayer->getInterpolatedValidValue(right_ndc.x(), right_ndc.y(), value);
                else validValue = elevationLayer->getValidValue(i-(_numColumns-1),j,value);
                ndc.z() = value*_scaleHeight;
            }

            if (validValue)
            {
                osg::Vec3d model;
                _masterLocator->convertLocalToModel(ndc, model);

                // compute the local normal
                osg::Vec3d ndc_one = ndc; ndc_one.z() += 1.0;
                osg::Vec3d model_one;
                _masterLocator->convertLocalToModel(ndc_one, model_one);
                model_one = model_one - model;
                model_one.normalize();

                setVertex(i, j, osg::Vec3(model-_centerModel), model_one);
                // OSG_NOTICE<<"       setVertex("<<i<<", "<<j<<"..)"<<std::endl;
            }
        }
    }
}

void VertexNormalGenerator::populateAboveBoundary(osgTerrain::Layer* elevationLayer)
{
    // OSG_NOTICE<<"   VertexNormalGenerator::populateAboveBoundary("<<elevationLayer<<")"<<std::endl;

    if (!elevationLayer) return;

    bool sampled = elevationLayer &&
                   ( (elevationLayer->getNumRows()!=static_cast<unsigned int>(_numRows)) ||
                     (elevationLayer->getNumColumns()!=static_cast<unsigned int>(_numColumns)) );

    for(int j=_numRows-1; j<_numRows+1; ++j)
    {
        for(int i=0; i<_numColumns; ++i)
        {
            osg::Vec3d ndc( ((double)i)/(double)(_numColumns-1), ((double)j)/(double)(_numRows-1), 0.0);
            osg::Vec3d above_ndc( ndc.x(), ndc.y()-1.0, 0.0);

            bool validValue = true;
            if (elevationLayer)
            {
                float value = 0.0f;
                if (sampled) validValue = elevationLayer->getInterpolatedValidValue(above_ndc.x(), above_ndc.y(), value);
                else validValue = elevationLayer->getValidValue(i,j-(_numRows-1),value);
                ndc.z() = value*_scaleHeight;
            }

            if (validValue)
            {
                osg::Vec3d model;
                _masterLocator->convertLocalToModel(ndc, model);

                // compute the local normal
                osg::Vec3d ndc_one = ndc; ndc_one.z() += 1.0;
                osg::Vec3d model_one;
                _masterLocator->convertLocalToModel(ndc_one, model_one);
                model_one = model_one - model;
                model_one.normalize();

                setVertex(i, j, osg::Vec3(model-_centerModel), model_one);
                // OSG_NOTICE<<"       setVertex("<<i<<", "<<j<<"..)"<<std::endl;
            }
        }
    }
}

void VertexNormalGenerator::populateBelowBoundary(osgTerrain::Layer* elevationLayer)
{
    // OSG_NOTICE<<"   VertexNormalGenerator::populateBelowBoundary("<<elevationLayer<<")"<<std::endl;

    if (!elevationLayer) return;

    bool sampled = elevationLayer &&
                   ( (elevationLayer->getNumRows()!=static_cast<unsigned int>(_numRows)) ||
                     (elevationLayer->getNumColumns()!=static_cast<unsigned int>(_numColumns)) );

    for(int j=-1; j<=0; ++j)
    {
        for(int i=0; i<_numColumns; ++i)
        {
            osg::Vec3d ndc( ((double)i)/(double)(_numColumns-1), ((double)j)/(double)(_numRows-1), 0.0);
            osg::Vec3d below_ndc( ndc.x(), 1.0+ndc.y(), 0.0);

            bool validValue = true;
            if (elevationLayer)
            {
                float value = 0.0f;
                if (sampled) validValue = elevationLayer->getInterpolatedValidValue(below_ndc.x(), below_ndc.y(), value);
                else validValue = elevationLayer->getValidValue(i,(_numRows-1)+j,value);
                ndc.z() = value*_scaleHeight;
            }

            if (validValue)
            {
                osg::Vec3d model;
                _masterLocator->convertLocalToModel(ndc, model);

                // compute the local normal
                osg::Vec3d ndc_one = ndc; ndc_one.z() += 1.0;
                osg::Vec3d model_one;
                _masterLocator->convertLocalToModel(ndc_one, model_one);
                model_one = model_one - model;
                model_one.normalize();

                setVertex(i, j, osg::Vec3(model-_centerModel), model_one);
                // OSG_NOTICE<<"       setVertex("<<i<<", "<<j<<"..)"<<std::endl;
            }
        }
    }
}


void VertexNormalGenerator::computeNormals()
{
    // compute normals for the center section
    for(int j=0; j<_numRows; ++j)
    {
        for(int i=0; i<_numColumns; ++i)
        {
            int vi = vertex_index(i, j);
            if (vi>=0) computeNormal(i, j, (*_normals)[vi]);
            else OSG_NOTICE<<"Not computing normal, vi="<<vi<<std::endl;
        }
    }
}

void VPBTechnique::generateGeometry(BufferData& buffer, Locator* masterLocator, const osg::Vec3d& centerModel)
{
    osg::Image* landclassImage = 0;
    SGMaterialCache::Atlas atlas;

    Terrain* terrain = _terrainTile->getTerrain();
    osgTerrain::Layer* elevationLayer = _terrainTile->getElevationLayer();
    osgTerrain::Layer* colorLayer = _terrainTile->getColorLayer(0);
    if (colorLayer) landclassImage = colorLayer->getImage();

    // Determine the correct Effect for this, based on a material lookup taking into account
    // the lat/lon of the center.
    SGMaterialLibPtr matlib  = _options->getMaterialLib();
    const SGGeod loc = computeCenterGeod(buffer, masterLocator);

    SGPropertyNode_ptr landEffectProp = new SGPropertyNode();
    SGPropertyNode_ptr waterEffectProp = new SGPropertyNode();

    if (matlib) {
      SG_LOG(SG_TERRAIN, SG_DEBUG, "Applying VPB material " << loc);
      SGMaterialCache* matcache = _options->getMaterialLib()->generateMatCache(loc, _options);
      atlas = matcache->getAtlas();
      SGMaterial* landmat = matcache->find("ws30land");
      SGMaterial* watermat = matcache->find("ws30water");
      delete matcache;

      if (landmat && watermat) {
        makeChild(landEffectProp.ptr(), "inherits-from")->setStringValue(landmat->get_effect_name());
        makeChild(waterEffectProp.ptr(), "inherits-from")->setStringValue(watermat->get_effect_name());
      } else {
        SG_LOG( SG_TERRAIN, SG_ALERT, "Unable to get effect for VPB - no matching material in library");
        makeChild(landEffectProp.ptr(), "inherits-from")->setStringValue("Effects/model-default");
        makeChild(waterEffectProp.ptr(), "inherits-from")->setStringValue("Effects/model-default");
      }
    } else {
        SG_LOG( SG_TERRAIN, SG_ALERT, "Unable to get effect for VPB - no material library available");
        makeChild(landEffectProp.ptr(), "inherits-from")->setStringValue("Effects/model-default");
        makeChild(waterEffectProp.ptr(), "inherits-from")->setStringValue("Effects/model-default");
    }

    buffer._landGeode = new EffectGeode();
    buffer._waterGeode = new EffectGeode();
    if(buffer._transform.valid()) {
        buffer._transform->addChild(buffer._landGeode.get());
        buffer._transform->addChild(buffer._waterGeode.get());
    }

    buffer._landGeometry = new osg::Geometry;
    buffer._landGeode->addDrawable(buffer._landGeometry.get());
  
    osg::ref_ptr<Effect> landEffect = makeEffect(landEffectProp, true, _options);
    buffer._landGeode->setEffect(landEffect.get());
    buffer._landGeode->setNodeMask(SG_NODEMASK_TERRAIN_BIT);

    buffer._waterGeometry = new osg::Geometry;
    buffer._waterGeode->addDrawable(buffer._waterGeometry.get());
  
    osg::ref_ptr<Effect> waterEffect = makeEffect(waterEffectProp, true, _options);
    buffer._waterGeode->setEffect(waterEffect.get());
    buffer._waterGeode->setNodeMask(SG_NODEMASK_TERRAIN_BIT);

    unsigned int numRows = 20;
    unsigned int numColumns = 20;

    if (elevationLayer)
    {
        numColumns = elevationLayer->getNumColumns();
        numRows = elevationLayer->getNumRows();
    }

    double scaleHeight = SGSceneFeatures::instance()->getVPBVerticalScale();
    double sampleRatio = SGSceneFeatures::instance()->getVPBSampleRatio();
    double constraint_gap = SGSceneFeatures::instance()->getVPBConstraintGap();

    unsigned int minimumNumColumns = 16u;
    unsigned int minimumNumRows = 16u;

    if ((sampleRatio!=1.0f) && (numColumns>minimumNumColumns) && (numRows>minimumNumRows))
    {
        unsigned int originalNumColumns = numColumns;
        unsigned int originalNumRows = numRows;

        numColumns = std::max((unsigned int) (float(originalNumColumns)*sqrtf(sampleRatio)), minimumNumColumns);
        numRows = std::max((unsigned int) (float(originalNumRows)*sqrtf(sampleRatio)),minimumNumRows);
    }

    bool treatBoundariesToValidDataAsDefaultValue = _terrainTile->getTreatBoundariesToValidDataAsDefaultValue();
    OSG_INFO<<"TreatBoundariesToValidDataAsDefaultValue="<<treatBoundariesToValidDataAsDefaultValue<<std::endl;

    float skirtHeight = 0.0f;
    HeightFieldLayer* hfl = dynamic_cast<HeightFieldLayer*>(elevationLayer);
    if (hfl && hfl->getHeightField())
    {
        skirtHeight = hfl->getHeightField()->getSkirtHeight();
    }

    bool createSkirt = skirtHeight != 0.0f;

    // construct the VertexNormalGenerator which will manage the generation and the vertices and normals
    VertexNormalGenerator VNG(masterLocator, centerModel, numRows, numColumns, scaleHeight, constraint_gap, createSkirt);

    unsigned int numVertices = VNG.capacity();

    // allocate and assign vertices
    buffer._landGeometry->setVertexArray(VNG._vertices.get());
    buffer._waterGeometry->setVertexArray(VNG._vertices.get());

    // allocate and assign normals
    buffer._landGeometry->setNormalArray(VNG._normals.get(), osg::Array::BIND_PER_VERTEX);
    buffer._waterGeometry->setNormalArray(VNG._normals.get(), osg::Array::BIND_PER_VERTEX);

    // allocate and assign color
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array(1);
    (*colors)[0].set(1.0f,1.0f,1.0f,1.0f);

    buffer._landGeometry->setColorArray(colors.get(), osg::Array::BIND_OVERALL);
    buffer._waterGeometry->setColorArray(colors.get(), osg::Array::BIND_OVERALL);

    // allocate and assign texture coordinates
    auto texcoords = new osg::Vec2Array;
    VNG.populateCenter(elevationLayer, texcoords);
    buffer._landGeometry->setTexCoordArray(0, texcoords);
    buffer._waterGeometry->setTexCoordArray(0, texcoords);

    if (terrain && terrain->getEqualizeBoundaries())
    {
        TileID tileID = _terrainTile->getTileID();

        osg::ref_ptr<TerrainTile> left_tile  = terrain->getTile(TileID(tileID.level, tileID.x-1, tileID.y));
        osg::ref_ptr<TerrainTile> right_tile = terrain->getTile(TileID(tileID.level, tileID.x+1, tileID.y));
        osg::ref_ptr<TerrainTile> top_tile = terrain->getTile(TileID(tileID.level, tileID.x, tileID.y+1));
        osg::ref_ptr<TerrainTile> bottom_tile = terrain->getTile(TileID(tileID.level, tileID.x, tileID.y-1));

        VNG.populateLeftBoundary(left_tile.valid() ? left_tile->getElevationLayer() : 0);
        VNG.populateRightBoundary(right_tile.valid() ? right_tile->getElevationLayer() : 0);
        VNG.populateAboveBoundary(top_tile.valid() ? top_tile->getElevationLayer() : 0);
        VNG.populateBelowBoundary(bottom_tile.valid() ? bottom_tile->getElevationLayer() : 0);

        _neighbours.clear();

        bool updateNeighboursImmediately = false;

        if (left_tile.valid())   addNeighbour(left_tile.get());
        if (right_tile.valid())  addNeighbour(right_tile.get());
        if (top_tile.valid())    addNeighbour(top_tile.get());
        if (bottom_tile.valid()) addNeighbour(bottom_tile.get());

        if (left_tile.valid())
        {
            if (left_tile->getTerrainTechnique()==0 || !(left_tile->getTerrainTechnique()->containsNeighbour(_terrainTile)))
            {
                int dirtyMask = left_tile->getDirtyMask() | TerrainTile::LEFT_EDGE_DIRTY;
                if (updateNeighboursImmediately) left_tile->init(dirtyMask, true);
                else left_tile->setDirtyMask(dirtyMask);
            }
        }
        if (right_tile.valid())
        {
            if (right_tile->getTerrainTechnique()==0 || !(right_tile->getTerrainTechnique()->containsNeighbour(_terrainTile)))
            {
                int dirtyMask = right_tile->getDirtyMask() | TerrainTile::RIGHT_EDGE_DIRTY;
                if (updateNeighboursImmediately) right_tile->init(dirtyMask, true);
                else right_tile->setDirtyMask(dirtyMask);
            }
        }
        if (top_tile.valid())
        {
            if (top_tile->getTerrainTechnique()==0 || !(top_tile->getTerrainTechnique()->containsNeighbour(_terrainTile)))
            {
                int dirtyMask = top_tile->getDirtyMask() | TerrainTile::TOP_EDGE_DIRTY;
                if (updateNeighboursImmediately) top_tile->init(dirtyMask, true);
                else top_tile->setDirtyMask(dirtyMask);
            }
        }

        if (bottom_tile.valid())
        {
            if (bottom_tile->getTerrainTechnique()==0 || !(bottom_tile->getTerrainTechnique()->containsNeighbour(_terrainTile)))
            {
                int dirtyMask = bottom_tile->getDirtyMask() | TerrainTile::BOTTOM_EDGE_DIRTY;
                if (updateNeighboursImmediately) bottom_tile->init(dirtyMask, true);
                else bottom_tile->setDirtyMask(dirtyMask);
            }
        }
    }

    osg::ref_ptr<osg::Vec3Array> skirtVectors = new osg::Vec3Array((*VNG._normals));
    VNG.computeNormals();

    //
    // populate the primitive data
    //
    bool swapOrientation = !(masterLocator->orientationOpenGL());
    bool smallTile = numVertices < 65536;

    // OSG_NOTICE<<"smallTile = "<<smallTile<<std::endl;

    osg::ref_ptr<osg::DrawElements> landElements = smallTile ?
        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_TRIANGLES)) :
        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_TRIANGLES));
    landElements->reserveElements((numRows-1) * (numColumns-1) * 6);

    buffer._landGeometry->addPrimitiveSet(landElements.get());

    osg::ref_ptr<osg::DrawElements> waterElements = smallTile ?
        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_TRIANGLES)) :
        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_TRIANGLES));
    waterElements->reserveElements((numRows-1) * (numColumns-1) * 6);
    buffer._waterGeometry->addPrimitiveSet(waterElements.get());

    unsigned int i, j;
    for(j=0; j<numRows-1; ++j)
    {
        for(i=0; i<numColumns-1; ++i)
        {
            // remap indices to final vertex positions
            int i00 = VNG.vertex_index(i,   j);
            int i01 = VNG.vertex_index(i,   j+1);
            int i10 = VNG.vertex_index(i+1, j);
            int i11 = VNG.vertex_index(i+1, j+1);

            if (swapOrientation)
            {
                std::swap(i00,i01);
                std::swap(i10,i11);
            }

            // Determine if this quad or triangle should be water or not, and therefore which geometry to add it to.
            bool w00 = false;
            bool w01 = false;
            bool w10 = false;
            bool w11 = false;
            
            if (landclassImage && matlib) {
                if (i00>=0) w00 = atlas.waterAtlas[int(round(landclassImage->getColor((*texcoords)[i00]).r() * 255.0))];
                if (i01>=0) w01 = atlas.waterAtlas[int(round(landclassImage->getColor((*texcoords)[i01]).r() * 255.0))];
                if (i10>=0) w10 = atlas.waterAtlas[int(round(landclassImage->getColor((*texcoords)[i10]).r() * 255.0))];
                if (i11>=0) w11 = atlas.waterAtlas[int(round(landclassImage->getColor((*texcoords)[i11]).r() * 255.0))];
            }

            unsigned int numValid = 0;
            if (i00>=0) ++numValid;
            if (i01>=0) ++numValid;
            if (i10>=0) ++numValid;
            if (i11>=0) ++numValid;

            if (numValid==4)
            {
                osg::ref_ptr<osg::DrawElements> elements;

                // optimize which way to put the diagonal by choosing to
                // place it between the two corners that have the least curvature
                // relative to each other.
                float dot_00_11 = (*VNG._normals)[i00] * (*VNG._normals)[i11];
                float dot_01_10 = (*VNG._normals)[i01] * (*VNG._normals)[i10];

                if (dot_00_11 > dot_01_10)
                {
                    elements = (w01 && w00 && w11) ? waterElements : landElements;
                    elements->addElement(i01);
                    elements->addElement(i00);
                    elements->addElement(i11);

                    elements = (w00 && w10 && w11) ? waterElements : landElements;
                    elements->addElement(i00);
                    elements->addElement(i10);
                    elements->addElement(i11);
                }
                else
                {
                    elements = (w01 && w00 && w10) ? waterElements : landElements;
                    elements->addElement(i01);
                    elements->addElement(i00);
                    elements->addElement(i10);

                    elements = (w01 && w10 && w11) ? waterElements : landElements;
                    elements->addElement(i01);
                    elements->addElement(i10);
                    elements->addElement(i11);
                }
            }
            else if (numValid==3)
            {
                // If this is a triangle, we need to look for exactly 3 our of the four
                // vertices to be in water, as the fourth will be false, as above.
                unsigned int water_count = 0;
                if (w00) water_count++;
                if (w01) water_count++;
                if (w10) water_count++;
                if (w11) water_count++;

                osg::ref_ptr<osg::DrawElements> elements = (water_count == 3) ? waterElements : landElements;
                if (i00>=0) elements->addElement(i00);
                if (i01>=0) elements->addElement(i01);
                if (i11>=0) elements->addElement(i11);
                if (i10>=0) elements->addElement(i10);
            }
        }
    }


    if (createSkirt)
    {
        osg::ref_ptr<osg::Vec3Array> vertices = VNG._vertices.get();
        osg::ref_ptr<osg::Vec3Array> normals = VNG._normals.get();

        osg::ref_ptr<osg::DrawElements> skirtDrawElements = smallTile ?
            static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
            static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));

        // create bottom skirt vertices
        int r,c;
        r=0;
        for(c=0;c<static_cast<int>(numColumns);++c)
        {
            int orig_i = VNG.vertex_index(c,r);
            if (orig_i>=0)
            {
                unsigned int new_i = vertices->size(); // index of new index of added skirt point
                osg::Vec3 new_v = (*vertices)[orig_i] - ((*skirtVectors)[orig_i])*skirtHeight;
                (*vertices).push_back(new_v);
                if (normals.valid()) (*normals).push_back((*normals)[orig_i]);

                texcoords->push_back((*texcoords)[orig_i]);

                skirtDrawElements->addElement(orig_i);
                skirtDrawElements->addElement(new_i);
            }
            else
            {
                if (skirtDrawElements->getNumIndices()!=0)
                {
                    buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
                    skirtDrawElements = smallTile ?
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));
                }

            }
        }

        if (skirtDrawElements->getNumIndices()!=0)
        {
            buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
            skirtDrawElements = smallTile ?
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));
        }

        // create right skirt vertices
        c=numColumns-1;
        for(r=0;r<static_cast<int>(numRows);++r)
        {
            int orig_i = VNG.vertex_index(c,r); // index of original vertex of grid
            if (orig_i>=0)
            {
                unsigned int new_i = vertices->size(); // index of new index of added skirt point
                osg::Vec3 new_v = (*vertices)[orig_i] - ((*skirtVectors)[orig_i])*skirtHeight;
                (*vertices).push_back(new_v);
                if (normals.valid()) (*normals).push_back((*normals)[orig_i]);
                texcoords->push_back((*texcoords)[orig_i]);

                skirtDrawElements->addElement(orig_i);
                skirtDrawElements->addElement(new_i);
            }
            else
            {
                if (skirtDrawElements->getNumIndices()!=0)
                {
                    buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
                    skirtDrawElements = smallTile ?
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));
                }

            }
        }

        if (skirtDrawElements->getNumIndices()!=0)
        {
            buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
            skirtDrawElements = smallTile ?
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));
        }

        // create top skirt vertices
        r=numRows-1;
        for(c=numColumns-1;c>=0;--c)
        {
            int orig_i = VNG.vertex_index(c,r); // index of original vertex of grid
            if (orig_i>=0)
            {
                unsigned int new_i = vertices->size(); // index of new index of added skirt point
                osg::Vec3 new_v = (*vertices)[orig_i] - ((*skirtVectors)[orig_i])*skirtHeight;
                (*vertices).push_back(new_v);
                if (normals.valid()) (*normals).push_back((*normals)[orig_i]);

                texcoords->push_back((*texcoords)[orig_i]);

                skirtDrawElements->addElement(orig_i);
                skirtDrawElements->addElement(new_i);
            }
            else
            {
                if (skirtDrawElements->getNumIndices()!=0)
                {
                    buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
                    skirtDrawElements = smallTile ?
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));
                }

            }
        }

        if (skirtDrawElements->getNumIndices()!=0)
        {
            buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
            skirtDrawElements = smallTile ?
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_QUAD_STRIP)) :
                        static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_QUAD_STRIP));
        }

        // create left skirt vertices
        c=0;
        for(r=numRows-1;r>=0;--r)
        {
            int orig_i = VNG.vertex_index(c,r); // index of original vertex of grid
            if (orig_i>=0)
            {
                unsigned int new_i = vertices->size(); // index of new index of added skirt point
                osg::Vec3 new_v = (*vertices)[orig_i] - ((*skirtVectors)[orig_i])*skirtHeight;
                (*vertices).push_back(new_v);
                if (normals.valid()) (*normals).push_back((*normals)[orig_i]);

                texcoords->push_back((*texcoords)[orig_i]);

                skirtDrawElements->addElement(orig_i);
                skirtDrawElements->addElement(new_i);
            }
            else
            {
                if (skirtDrawElements->getNumIndices()!=0)
                {
                    buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
                    skirtDrawElements = new osg::DrawElementsUShort(GL_QUAD_STRIP);
                }
            }
        }

        if (skirtDrawElements->getNumIndices()!=0)
        {
            buffer._landGeometry->addPrimitiveSet(skirtDrawElements.get());
        }
    }

    waterElements->resizeElements(waterElements->getNumIndices());
    landElements->resizeElements(landElements->getNumIndices());

    buffer._landGeometry->setUseDisplayList(false);
    buffer._landGeometry->setUseVertexBufferObjects(true);
    buffer._waterGeometry->setUseDisplayList(false);
    buffer._waterGeometry->setUseVertexBufferObjects(true);
    buffer._landGeode->runGenerators(buffer._landGeometry);
    buffer._waterGeode->runGenerators(buffer._waterGeometry);

    // Tile-specific information for the shaders
    osg::StateSet *landStateSet = buffer._landGeode->getOrCreateStateSet();
    osg::StateSet *waterStateSet = buffer._waterGeode->getOrCreateStateSet();
    osg::ref_ptr<osg::Uniform> level = new osg::Uniform("tile_level", _terrainTile->getTileID().level);
    landStateSet->addUniform(level);
    waterStateSet->addUniform(level);

    // Determine the x and y texture scaling.  Has to be performed after we've generated all the vertices.
    // Because the earth is round, each tile is not a rectangle.  Apart from edge cases like the poles, the
    // difference in axis length is < 1%, so we will just take the average.
    // Note that we can ignore the actual texture coordinates as we know from above that they are always
    // [0..1.0] [0..1.0] across the entire tile.
    osg::Vec3f bottom_left, bottom_right, top_left, top_right;
    bool got_bl = VNG.vertex(0, 0, bottom_left);
    bool got_br = VNG.vertex(0, VNG._numColumns - 1, bottom_right);
    bool got_tl = VNG.vertex(VNG._numColumns - 1, 0, top_left);
    bool got_tr = VNG.vertex(VNG._numColumns - 1, VNG._numRows -1, top_right);

    if (got_bl && got_br && got_tl && got_tr) {
        osg::Vec3f s = bottom_right - bottom_left;
        osg::Vec3f t = top_left - bottom_left;
        osg::Vec3f u = top_right - top_left;
        osg::Vec3f v = top_right - bottom_right;
        buffer._width = 0.5 * (s.length() + u.length());
        buffer._height = 0.5 * (t.length() + v.length());
    }

    SG_LOG(SG_TERRAIN, SG_DEBUG, "Tile Level " << _terrainTile->getTileID().level << " width " << buffer._width << " height " << buffer._height);

    osg::ref_ptr<osg::Uniform> twu = new osg::Uniform("tile_width", buffer._width);
    landStateSet->addUniform(twu);
    waterStateSet->addUniform(twu);
    osg::ref_ptr<osg::Uniform> thu = new osg::Uniform("tile_height", buffer._height);
    landStateSet->addUniform(thu);
    waterStateSet->addUniform(thu);

    // Force build of KD trees?
    if (osgDB::Registry::instance()->getBuildKdTreesHint()==osgDB::ReaderWriter::Options::BUILD_KDTREES &&
        osgDB::Registry::instance()->getKdTreeBuilder())
    {

        //osg::Timer_t before = osg::Timer::instance()->tick();
        //OSG_NOTICE<<"osgTerrain::VPBTechnique::build kd tree"<<std::endl;
        osg::ref_ptr<osg::KdTreeBuilder> builder = osgDB::Registry::instance()->getKdTreeBuilder()->clone();
        buffer._landGeode->accept(*builder);
        buffer._waterGeode->accept(*builder);
        //osg::Timer_t after = osg::Timer::instance()->tick();
        //OSG_NOTICE<<"KdTree build time "<<osg::Timer::instance()->delta_m(before, after)<<std::endl;
    }
}

void VPBTechnique::applyColorLayers(BufferData& buffer, Locator* masterLocator)
{   

    const SGPropertyNode* propertyNode = _options->getPropertyNode().get();
    bool photoScenery = false;

    if (propertyNode) {
        photoScenery = _options->getPropertyNode()->getBoolValue("/sim/rendering/photoscenery/enabled");
    }

    if (photoScenery) {
        // Photoscenery is enabled, so we need to find and assign the orthophoto texture

        // Firstly, we need to work out the texture file we want to load.  Fortunately this follows the same
        // naming convention as the VPB scenery itself.
        auto tileID = _terrainTile->getTileID();
        SG_LOG(SG_TERRAIN, SG_DEBUG, "Using Photoscenery for " << _fileName << " " << tileID.level << " X" << tileID.x << " Y" << tileID.y);

        const osg::Vec3d world = buffer._transform->getMatrix().getTrans();
        const SGGeod loc = SGGeod::fromCart(toSG(world));
        const SGBucket bucket = SGBucket(loc);
        SGPath orthotexture;

        osgDB::FilePathList& pathList = _options->getDatabasePathList();
        bool found = false;

        for (auto iter = pathList.begin(); !found && iter != pathList.end(); ++iter) {
            orthotexture = SGPath(*iter);
            orthotexture.append("Orthophotos");
            orthotexture.append(bucket.gen_vpb_subtile(tileID.level, tileID.x, tileID.y) + ".dds");
            SG_LOG(SG_TERRAIN, SG_DEBUG, "Looking for phototexture " << orthotexture);

            if (orthotexture.exists()) {
                found = true;
                SG_LOG(SG_TERRAIN, SG_DEBUG, "Found phototexture " << orthotexture);
            }
        }

        if (found) {
            // Set up the texture with wrapping of UV to reduce black edges at tile boundaries.
            osg::Texture2D* texture = SGLoadTexture2D(SGPath(orthotexture), _options, true, true);
            osg::StateSet* stateset = buffer._landGeode->getOrCreateStateSet();
            stateset->setTextureAttributeAndModes(0, texture);
            stateset->addUniform(new osg::Uniform("photoScenery", true));
        } else {
            SG_LOG(SG_TERRAIN, SG_DEBUG, "Unable to find " << orthotexture);
            photoScenery = false;
        }
    }

    if (!photoScenery) {
        // Either photoscenery is turned off, or we failed to find a suitable texture.
        SGMaterialLibPtr matlib  = _options->getMaterialLib();
        if (!matlib) return;

        osgTerrain::Layer* colorLayer = _terrainTile->getColorLayer(0);
        if (!colorLayer) return;

        osg::Image* image = colorLayer->getImage();
        if (!image || ! image->valid()) return;

        // First time generating this texture, so process to change landclass IDs to texure indexes.
        SGPropertyNode_ptr landEffectProp;

        const SGGeod loc = computeCenterGeod(buffer, masterLocator);
        SG_LOG(SG_TERRAIN, SG_DEBUG, "Applying VPB material " << loc);

        SGMaterialCache::Atlas atlas = matlib->generateMatCache(loc, _options)->getAtlas();
        SGMaterialCache::AtlasIndex atlasIndex = atlas.index;

        // Set the "g" color channel to an index into the atlas index.
        for (unsigned int s = 0; s < (unsigned int) image->s(); s++) {
            for (unsigned int t = 0; t < (unsigned int) image->t(); t++) {
                osg::Vec4d c = image->getColor(s, t);
                int i = int(round(c.x() * 255.0));
                c.set(c.x(), (double) (atlasIndex[i] / 255.0), c.z(), c.w() );
                image->setColor(c, s, t);
            }
        }

        SG_LOG(SG_TERRAIN, SG_DEBUG, "VPB Image level:" << _terrainTile->getTileID().level << " " << image->s() << "x" << image->t() << " mipmaps:" << image->getNumMipmapLevels() << " format:" << image->getInternalTextureFormat());

        osg::ref_ptr<osg::Texture2D> texture2D  = new osg::Texture2D;
        texture2D->setImage(image);
        texture2D->setMaxAnisotropy(16.0f);
        texture2D->setResizeNonPowerOfTwoHint(false);

        // Use mipmaps only in the minimization case because on magnification this results
        // in bad interpolation of boundaries between landclasses
        texture2D->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST_MIPMAP_NEAREST);
        texture2D->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);

        texture2D->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
        texture2D->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);

        osg::StateSet* stateset = buffer._landGeode->getOrCreateStateSet();
        stateset->setTextureAttributeAndModes(0, texture2D, osg::StateAttribute::ON);
        stateset->setTextureAttributeAndModes(1, atlas.image, osg::StateAttribute::ON);
        stateset->addUniform(new osg::Uniform("photoScenery", false));
        stateset->addUniform(atlas.dimensions);
        stateset->addUniform(atlas.ambient);
        stateset->addUniform(atlas.diffuse);
        stateset->addUniform(atlas.specular);
    }
}

double VPBTechnique::det2(const osg::Vec2d a, const osg::Vec2d b)
{
    return a.x() * b.y() - b.x() * a.y();
}

void VPBTechnique::applyMaterials(BufferData& buffer, Locator* masterLocator)
{
    pc_init(2718281);

    // Define all possible handlers
    VegetationHandler vegetationHandler;
    RandomLightsHandler lightsHandler;
    std::vector<VPBMaterialHandler *> all_handlers{&vegetationHandler,
                                                   &lightsHandler};

    // Filter out handlers that do not apply to the current tile
    std::vector<VPBMaterialHandler *> handlers;
    for (const auto handler : all_handlers) {
        if (handler->initialize(_options, _terrainTile)) {
            handlers.push_back(handler);
        }
    }

    // If no handlers are relevant to the current tile, return immediately
    if (handlers.size() == 0) {
        return;
    }

    SGMaterialLibPtr matlib  = _options->getMaterialLib();
    SGMaterial* mat = 0;

    if (!matlib) return;

    const SGGeod loc = computeCenterGeod(buffer, masterLocator);
    SGMaterialCache* matcache = matlib->generateMatCache(loc, _options);

    osg::Vec3d up = buffer._transform->getMatrix().getTrans();
    up.normalize();

    const osg::Vec3d world = buffer._transform->getMatrix().getTrans();
    const SGGeoc cloc = SGGeoc::fromCart(toSG(world));

    if (!matcache) return;

    const osg::Matrixd R_vert = osg::Matrixd::rotate(
     M_PI / 2.0 - loc.getLatitudeRad(), osg::Vec3d(0.0, 1.0, 0.0),
     loc.getLongitudeRad(), osg::Vec3d(0.0, 0.0, 1.0),
     0.0, osg::Vec3d(1.0, 0.0, 0.0));

    const osg::Array* vertices = buffer._landGeometry->getVertexArray();
    const osg::Array* texture_coords = buffer._landGeometry->getTexCoordArray(0);
    osgTerrain::Layer* colorLayer = _terrainTile->getColorLayer(0);
    osg::Image* image = colorLayer->getImage();

    if (!image || ! image->valid()) {
        SG_LOG(SG_TERRAIN, SG_ALERT, "No landclass image for " << _terrainTile->getTileID().x << " " << _terrainTile->getTileID().y << " " << _terrainTile->getTileID().level);
        return;
    }

    const osg::Vec3* vertexPtr = static_cast<const osg::Vec3*>(vertices->getDataPointer());
    const osg::Vec2* texPtr = static_cast<const osg::Vec2*>(texture_coords->getDataPointer());

    const osg::PrimitiveSet* primSet = buffer._landGeometry->getPrimitiveSet(0);
    const osg::DrawElements* drawElements = primSet->getDrawElements();
    const unsigned int triangle_count = drawElements->getNumPrimitives();

    const double lon          = loc.getLongitudeRad();
    const double lat          = loc.getLatitudeRad();
    const double clon         = cloc.getLongitudeRad();
    const double clat         = cloc.getLatitudeRad();
    const double r_E_lat      = /* 6356752.3 */ 6.375993e+06;
    const double r_E_lon      = /* 6378137.0 */ 6.389377e+06;
    const double C            = r_E_lon * cos(lat);
    const double one_over_C   = (fabs(C) > 1.0e-4) ? (1.0 / C) : 0.0;
    const double one_over_r_E = 1.0 / r_E_lat;

    const osg::Matrix rotation_vertices_c = osg::Matrix::rotate(
     M_PI / 2 - clat, osg::Vec3d(0.0, 1.0, 0.0),
     clon, osg::Vec3d(0.0, 0.0, 1.0),
     0.0, osg::Vec3d(1.0, 0.0, 0.0));

    const osg::Matrix rotation_vertices_g = osg::Matrix::rotate(
     M_PI / 2 - lat, osg::Vec3d(0.0, 1.0, 0.0),
     lon, osg::Vec3d(0.0, 0.0, 1.0),
     0.0, osg::Vec3d(1.0, 0.0, 0.0));

    // Compute lat/lon deltas for each handler
    std::vector<std::pair<double, double>> deltas;
    for (const auto handler : handlers) {
        handler->setLocation(loc, r_E_lat, r_E_lon);
        deltas.push_back(
            std::make_pair(handler->get_delta_lat(), handler->get_delta_lon()));
    }

    // At the detailed tile level we are handling various materials, and
    // as we walk across the tile in a scanline, the landclass doesn't
    // change regularly from point to point.  Cache the required
    // material information for the current landclass to reduce the
    // number of lookups into the material cache.
    int current_land_class = -1;
    osg::Texture2D* object_mask = NULL;
    osg::Image* object_mask_image = NULL;
    float x_scale = 1000.0;
    float y_scale = 1000.0;

    for (unsigned int i = 0; i < triangle_count; i++)
    {
        const int i0 = drawElements->index(3 * i);
        const int i1 = drawElements->index(3 * i + 1);
        const int i2 = drawElements->index(3 * i + 2);

        const osg::Vec3 v0 = vertexPtr[i0];
        const osg::Vec3 v1 = vertexPtr[i1];
        const osg::Vec3 v2 = vertexPtr[i2];

        const osg::Vec3d v_0 = v0;
        const osg::Vec3d v_x = v1 - v0;
        const osg::Vec3d v_y = v2 - v0;

        osg::Vec3 n = v_x ^ v_y;
        n.normalize();

        const osg::Vec3d v_0_g = R_vert * v0;
        const osg::Vec3d v_1_g = R_vert * v1;
        const osg::Vec3d v_2_g = R_vert * v2;

        const osg::Vec2d ll_0 = osg::Vec2d(v_0_g.y() * one_over_C + lon, -v_0_g.x() * one_over_r_E + lat);
        const osg::Vec2d ll_1 = osg::Vec2d(v_1_g.y() * one_over_C + lon, -v_1_g.x() * one_over_r_E + lat);
        const osg::Vec2d ll_2 = osg::Vec2d(v_2_g.y() * one_over_C + lon, -v_2_g.x() * one_over_r_E + lat);

        const osg::Vec2d ll_O = ll_0;
        const osg::Vec2d ll_x = osg::Vec2d((v_1_g.y() - v_0_g.y()) * one_over_C, -(v_1_g.x() - v_0_g.x()) * one_over_r_E);
        const osg::Vec2d ll_y = osg::Vec2d((v_2_g.y() - v_0_g.y()) * one_over_C, -(v_2_g.x() - v_0_g.x()) * one_over_r_E);

        // Each handler may have a different delta/granularity in the scanline.
        // To take advantage of the material caching, we first collect all the
        // scan points from all the handlers for the current tile, and then scan
        // them in spatial order, calling the appropriate handler for each
        // point.
        //
        // We will insert <lon, lat, handler*> elements in a vector, and sort
        // all elements in increasing lon followed by increase lat, mimicking a
        // scanline reading approach for efficient landclass caching
        std::vector<std::tuple<double, double, VPBMaterialHandler *>>
            scan_points;

        for(auto iter=0u; iter!=handlers.size(); iter++) {
            const double delta_lat = deltas[iter].first;
            const double delta_lon = deltas[iter].second;
            const int off_x = ll_O.x() / delta_lon;
            const int off_y = ll_O.y() / delta_lat;
            const int min_lon = min(min(ll_0.x(), ll_1.x()), ll_2.x()) / delta_lon;
            const int max_lon = max(max(ll_0.x(), ll_1.x()), ll_2.x()) / delta_lon;
            const int min_lat = min(min(ll_0.y(), ll_1.y()), ll_2.y()) / delta_lat;
            const int max_lat = max(max(ll_0.y(), ll_1.y()), ll_2.y()) / delta_lat;

            for (int lat_int = min_lat - 1; lat_int <= max_lat + 1; lat_int++) {
                const double lat = (lat_int - off_y) * delta_lat;
                for (int lon_int = min_lon - 1; lon_int <= max_lon + 1;
                     lon_int++) {
                    const double lon = (lon_int - off_x) * delta_lon;
                    scan_points.push_back(
                        std::make_tuple(lon, lat, handlers[iter]));
                }
            }
        }

        std::sort(scan_points.begin(), scan_points.end());

        const osg::Vec2 t0 = texPtr[i0];
        const osg::Vec2 t1 = texPtr[i1];
        const osg::Vec2 t2 = texPtr[i2];

        const osg::Vec2d t_0 = t0;
        const osg::Vec2d t_x = t1 - t0;
        const osg::Vec2d t_y = t2 - t0;

        const double D = det2(ll_x, ll_y);

        for(auto const &point : scan_points) {
            const double lon = get<0>(point);
            const double lat = get<1>(point);
            VPBMaterialHandler *handler = get<2>(point);

            osg::Vec2d p(lon, lat);
            double x = det2(ll_x, p) / D;
            double y = det2(p, ll_y) / D;

            if ((x < 0.0) || (y < 0.0) || (x + y > 1.0)) continue;

            if (!image) {
                SG_LOG(SG_TERRAIN, SG_ALERT, "Image disappeared under my feet.");
                continue;
            }

            osg::Vec2 t = osg::Vec2(t_0 + t_x * x + t_y * y);
            unsigned int tx = (unsigned int) (image->s() * t.x()) % image->s();
            unsigned int ty = (unsigned int) (image->t() * t.y()) % image->t();
            const osg::Vec4 tc = image->getColor(tx, ty);
            const int land_class = int(round(tc.x() * 255.0));

            if (land_class != current_land_class) {
                // Use temporal locality to reduce material lookup by caching
                // some elements for future lookups against the same landclass.
                mat = matcache->find(land_class);
                if (!mat) continue;

                current_land_class = land_class;

                // We need to notify all handlers of material change, but
                // only consider the current handler being processed for
                // skipping the loop
                bool current_handler_result = true;
                for (const auto temp_handler : handlers) {
                    bool result = temp_handler->handleNewMaterial(mat);

                    if (temp_handler == handler) {
                        current_handler_result = result;
                    }
                }

                if (!current_handler_result) {
                    continue;
                }

                object_mask = mat->get_one_object_mask(0);
                object_mask_image = NULL;
                if (object_mask != NULL) {
                    object_mask_image = object_mask->getImage();
                    if (!object_mask_image || ! object_mask_image->valid()) {
                        object_mask_image = NULL;
                        continue;
                    }

                    // Texture coordinates run [0..1][0..1] across the entire tile whereas
                    // the texure itself has defined dimensions in m.
                    // We therefore need to use the tile width and height to determine the correct
                    // texture coordinate transformation.
                    x_scale = buffer._width / 1000.0;
                    y_scale = buffer._height / 1000.0;

                    if (mat->get_xsize() > 0.0) { x_scale = buffer._width / mat->get_xsize(); }
                    if (mat->get_ysize() > 0.0) { y_scale = buffer._height / mat->get_ysize(); }
                }
            }

            if (!mat) continue;

            handler->handleIteration(mat, object_mask_image,
                                     _randomObjectsConstraintGroup, lon, lat, p,
                                     D, ll_O, ll_x, ll_y, t_0, t_x, t_y, v_0,
                                     v_x, v_y, x_scale, y_scale, n, up);
        }
    }

    for (const auto handler : handlers) {
        handler->finish(_options, buffer._transform, loc);
    }
}

void VPBTechnique::applyLineFeatures(BufferData& buffer, Locator* masterLocator)
{
    unsigned int line_features_lod_range = 6;
    float minWidth = 9999.9;

    const unsigned int tileLevel = _terrainTile->getTileID().level;
    const SGPropertyNode* propertyNode = _options->getPropertyNode().get();

    if (propertyNode) {
        const SGPropertyNode* static_lod = propertyNode->getNode("/sim/rendering/static-lod");
        line_features_lod_range = static_lod->getIntValue("line-features-lod-level", line_features_lod_range);
        if (static_lod->getChildren("lod-level").size() > tileLevel) {
            minWidth = static_lod->getChildren("lod-level")[tileLevel]->getFloatValue("line-features-min-width", minWidth);
        }
    }

    if (tileLevel < line_features_lod_range) {
        // Do not generate vegetation for tiles too far away
        return;
    }

    SG_LOG(SG_TERRAIN, SG_DEBUG, "Generating roads of width > " << minWidth << " for tile LoD level " << tileLevel);

    const SGMaterialLibPtr matlib  = _options->getMaterialLib();
    SGMaterial* mat = 0;

    if (! matlib) {
        SG_LOG(SG_TERRAIN, SG_ALERT, "Unable to get materials library to generate roads");
        return;
    }    

    // Get all appropriate roads.  We assume that the VPB terrain tile is smaller than a Bucket size.
    LightBin lightbin;
    const osg::Vec3d world = buffer._transform->getMatrix().getTrans();
    const SGGeod loc = SGGeod::fromCart(toSG(world));
    const SGBucket bucket = SGBucket(loc);
    string material_name = "";
    auto roads = std::find_if(_lineFeatureLists.begin(), _lineFeatureLists.end(), [bucket](BucketLineFeatureBinList b){return (b.first == bucket);});

    if (roads == _lineFeatureLists.end()) return;

    SGMaterialCache* matcache = matlib->generateMatCache(loc, _options);

    for (; roads != _lineFeatureLists.end(); ++roads) {
        const LineFeatureBinList roadBins = roads->second;

        for (auto rb = roadBins.begin(); rb != roadBins.end(); ++rb)
        {
            if (material_name != rb->getMaterial()) {
                // Cache the material to reduce lookups.
                mat = matcache->find(rb->getMaterial());
                material_name = rb->getMaterial();
            }

            if (!mat) {
                SG_LOG(SG_TERRAIN, SG_ALERT, "Unable to find material " << rb->getMaterial() << " at " << loc << " " << bucket);
                continue;
            }    

            const unsigned int ysize = mat->get_ysize();
            const bool   light_edge_offset = mat->get_light_edge_offset();
            const double light_edge_spacing = mat->get_light_edge_spacing_m();
            const double light_edge_height = mat->get_light_edge_height_m();
            const double x0 = mat->get_line_feature_tex_x0();
            const double x1 = mat->get_line_feature_tex_x1();
            const double elevation_offset_m = mat->get_line_feature_offset_m();

            //  Generate a geometry for this set of roads.
            osg::Vec3Array* v = new osg::Vec3Array;
            osg::Vec2Array* t = new osg::Vec2Array;
            osg::Vec3Array* n = new osg::Vec3Array;
            osg::Vec4Array* c = new osg::Vec4Array;
            osg::Vec3Array* lights = new osg::Vec3Array;

            auto lineFeatures = rb->getLineFeatures();

            for (auto r = lineFeatures.begin(); r != lineFeatures.end(); ++r) {
                if (r->_width > minWidth) generateLineFeature(buffer, masterLocator, *r, world, v, t, n, lights, x0, x1, ysize, light_edge_spacing, light_edge_height, light_edge_offset, elevation_offset_m);
            }

            if (v->size() == 0) continue;

            c->push_back(osg::Vec4(1.0,1.0,1.0,1.0));

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(v);
            geometry->setTexCoordArray(0, t, osg::Array::BIND_PER_VERTEX);
            geometry->setTexCoordArray(1, t, osg::Array::BIND_PER_VERTEX);
            geometry->setNormalArray(n, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(c, osg::Array::BIND_OVERALL);
            geometry->setUseDisplayList( false );
            geometry->setUseVertexBufferObjects( true );
            geometry->addPrimitiveSet( new osg::DrawArrays( GL_TRIANGLES, 0, v->size()) );

            EffectGeode* geode = new EffectGeode;
            geode->addDrawable(geometry);

            geode->setMaterial(mat);
            geode->setEffect(mat->get_one_effect(0));
            geode->runGenerators(geometry);
            geode->setNodeMask(SG_NODEMASK_TERRAIN_BIT);
            buffer._transform->addChild(geode);
            addRandomObjectsConstraint(geode);

            if (lights->size() > 0) {
                const double size = mat->get_light_edge_size_cm();
                const double intensity = mat->get_light_edge_intensity_cd();
                const SGVec4f color = mat->get_light_edge_colour();
                const double horiz = mat->get_light_edge_angle_horizontal_deg();
                const double vertical = mat->get_light_edge_angle_vertical_deg();
                // Assume street lights point down.
                osg::Vec3d up = world;
                up.normalize();
                const SGVec3f direction = toSG(- (osg::Vec3f) up);

                std::for_each(lights->begin(), lights->end(), 
                    [&, size, intensity, color, direction, horiz, vertical] (osg::Vec3f p) { lightbin.insert(toSG(p), size, intensity, 1, color, direction, horiz, vertical); } );
            }
        }
    }

    if (lightbin.getNumLights() > 0) buffer._transform->addChild(createLights(lightbin, osg::Matrix::identity(), _options));
}

void VPBTechnique::generateLineFeature(BufferData& buffer, Locator* masterLocator, LineFeatureBin::LineFeature road, osg::Vec3d modelCenter, osg::Vec3Array* v, osg::Vec2Array* t, osg::Vec3Array* n, osg::Vec3Array* lights, double x0, double x1, unsigned int ysize, double light_edge_spacing, double light_edge_height, bool light_edge_offset, double elevation_offset_m)
{
    // We're in Earth-centered coordinates, so "up" is simply directly away from (0,0,0)
    osg::Vec3d up = modelCenter;
    up.normalize();
    TileBounds tileBounds(masterLocator, up);

    std::list<osg::Vec3d> nodes = tileBounds.clipToTile(road._nodes);

    // We need at least two node to make a road.
    if (nodes.size() < 2) return; 

    osg::Vec3d ma, mb;
    std::list<osg::Vec3d> roadPoints;
    auto road_iter = nodes.begin();

    ma = getMeshIntersection(buffer, masterLocator, *road_iter - modelCenter, up);
    road_iter++;

    for (; road_iter != nodes.end(); road_iter++) {
        mb = getMeshIntersection(buffer, masterLocator, *road_iter - modelCenter, up);
        auto esl = VPBElevationSlice::computeVPBElevationSlice(buffer._landGeometry, ma, mb, up);

        for(auto eslitr = esl.begin(); eslitr != esl.end(); ++eslitr) {
            roadPoints.push_back(*eslitr);
        }

        // Now traverse the next segment
        ma = mb;
    }

    if (roadPoints.size() == 0) return;

    // We now have a series of points following the topography of the elevation mesh.

    auto iter = roadPoints.begin();
    osg::Vec3d start = *iter;
    iter++;

    osg::Vec3d last_spanwise =  (*iter - start)^ up;
    last_spanwise.normalize();

    float yTexBaseA = 0.0f;
    float yTexBaseB = 0.0f;
    float last_light_distance = 0.0f;

    for (; iter != roadPoints.end(); iter++) {

        osg::Vec3d end = *iter;

        // Ignore tiny segments - likely artifacts of the elevation slicer
        if ((end - start).length2() < 1.0) continue;

        // Find a spanwise vector
        osg::Vec3d spanwise = ((end-start) ^ up);
        spanwise.normalize();

        // Define the road extents
        const osg::Vec3d a = start - last_spanwise * road._width * 0.5 + up * elevation_offset_m;
        const osg::Vec3d b = start + last_spanwise * road._width * 0.5 + up * elevation_offset_m;
        const osg::Vec3d c = end   - spanwise * road._width * 0.5 + up * elevation_offset_m;
        const osg::Vec3d d = end   + spanwise * road._width * 0.5 + up * elevation_offset_m;

        // Determine the x and y texture coordinates for the edges
        const float yTexA = yTexBaseA + (c-a).length() / ysize;
        const float yTexB = yTexBaseB + (d-b).length() / ysize;

        // Now generate two triangles, .
        v->push_back(a);
        v->push_back(b);
        v->push_back(c);

        t->push_back(osg::Vec2d(x0, yTexBaseA));
        t->push_back(osg::Vec2d(x1, yTexBaseB));
        t->push_back(osg::Vec2d(x0, yTexA));

        v->push_back(b);
        v->push_back(d);
        v->push_back(c);

        t->push_back(osg::Vec2d(x1, yTexBaseB));
        t->push_back(osg::Vec2d(x1, yTexB));
        t->push_back(osg::Vec2d(x0, yTexA));

        // Normal is straight from the quad
        osg::Vec3d normal = -(end-start)^spanwise;
        normal.normalize();
        for (unsigned int i = 0; i < 6; i++) n->push_back(normal);

        start = end;
        yTexBaseA = yTexA;
        yTexBaseB = yTexB;
        last_spanwise = spanwise;
        float edge_length = (c-a).length();
        float start_a = last_light_distance;
        float start_b = start_a;

        if ((road._attributes == 1) && (light_edge_spacing > 0.0)) {
            // We have some edge lighting.  Traverse edges a-c and b-d adding lights as appropriate.
            
            // Handle the case where lights are on alternate sides of the road rather than in pairs
            if (light_edge_offset) start_b = fmodf(start_b + light_edge_spacing * 0.5, light_edge_spacing);

            osg::Vec3f p1 = (c-a);
            p1.normalize();

            while (start_a < edge_length) {
                lights->push_back(a + (osg::Vec3f) p1 * start_a + up * (light_edge_height + 1.0));
                start_a += light_edge_spacing;
            }

            osg::Vec3f p2 = (d-b);
            p2.normalize();

            while (start_b < edge_length) {
                lights->push_back(b + (osg::Vec3f) p2 * start_b + up * (light_edge_height + 1.0));
                start_b += light_edge_spacing;
            }

            // Determine the position for the first light on the next road segment.
            last_light_distance = fmodf(start_a + edge_length, light_edge_spacing);
        }
    }
}

void VPBTechnique::applyAreaFeatures(BufferData& buffer, Locator* masterLocator)
{
    unsigned int area_features_lod_range = 6;
    float minArea = 1000.0;

    const unsigned int tileLevel = _terrainTile->getTileID().level;
    const SGPropertyNode* propertyNode = _options->getPropertyNode().get();

    if (propertyNode) {
        const SGPropertyNode* static_lod = propertyNode->getNode("/sim/rendering/static-lod");
        area_features_lod_range = static_lod->getIntValue("area-features-lod-level", area_features_lod_range);
        if (static_lod->getChildren("lod-level").size() > tileLevel) {
            minArea = static_lod->getChildren("lod-level")[tileLevel]->getFloatValue("area-features-min-width", minArea);
            minArea *= minArea;
        }
    }

    if (tileLevel < area_features_lod_range) {
        // Do not generate areas for tiles too far away
        return;
    }

    const SGMaterialLibPtr matlib  = _options->getMaterialLib();
    SGMaterial* mat = 0;

    if (! matlib) {
        SG_LOG(SG_TERRAIN, SG_ALERT, "Unable to get materials library to generate areas");
        return;
    }    


    // Get all appropriate areas.  We assume that the VPB terrain tile is smaller than a Bucket size.
    const osg::Vec3d world = buffer._transform->getMatrix().getTrans();
    const SGGeod loc = SGGeod::fromCart(toSG(world));
    const SGBucket bucket = SGBucket(loc);
    auto areas = std::find_if(_areaFeatureLists.begin(), _areaFeatureLists.end(), [bucket](BucketAreaFeatureBinList b){return (b.first == bucket);});

    if (areas == _areaFeatureLists.end()) return;

    SGMaterialCache* matcache = _options->getMaterialLib()->generateMatCache(loc, _options);

    for (; areas != _areaFeatureLists.end(); ++areas) {
        const AreaFeatureBinList areaBins = areas->second;

        for (auto rb = areaBins.begin(); rb != areaBins.end(); ++rb)
        {
            mat = matcache->find(rb->getMaterial());

            if (!mat) {
                SG_LOG(SG_TERRAIN, SG_ALERT, "Unable to find material " << rb->getMaterial() << " at " << loc << " " << bucket);
                continue;
            }    

            unsigned int xsize = mat->get_xsize();
            unsigned int ysize = mat->get_ysize();

            //  Generate a geometry for this set of areas.
            osg::Vec3Array* v = new osg::Vec3Array;
            osg::Vec2Array* t = new osg::Vec2Array;
            osg::Vec3Array* n = new osg::Vec3Array;
            osg::Vec4Array* c = new osg::Vec4Array;

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(v);
            geometry->setTexCoordArray(0, t, osg::Array::BIND_PER_VERTEX);
            geometry->setTexCoordArray(1, t, osg::Array::BIND_PER_VERTEX);
            geometry->setNormalArray(n, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(c, osg::Array::BIND_OVERALL);
            geometry->setUseDisplayList( false );
            geometry->setUseVertexBufferObjects( true );

            auto areaFeatures = rb->getAreaFeatures();

            for (auto r = areaFeatures.begin(); r != areaFeatures.end(); ++r) {
                if (r->_area > minArea) generateAreaFeature(buffer, masterLocator, *r, world, geometry, v, t, n, xsize, ysize);
            }

            if (v->size() == 0) continue;
            c->push_back(osg::Vec4(1.0,1.0,1.0,1.0));

            geometry->dirtyBound();

            EffectGeode* geode = new EffectGeode;
            geode->addDrawable(geometry);

            geode->setMaterial(mat);
            geode->setEffect(mat->get_one_effect(0));
            geode->setNodeMask(SG_NODEMASK_TERRAIN_BIT);
            buffer._transform->addChild(geode);
        }
    }
}

void VPBTechnique::generateAreaFeature(BufferData& buffer, Locator* masterLocator, AreaFeatureBin::AreaFeature area, osg::Vec3d modelCenter, osg::Geometry* geometry, osg::Vec3Array* v, osg::Vec2Array* t, osg::Vec3Array* n, unsigned int xsize, unsigned int ysize)
{
    if (area._nodes.size() < 3) { 
        SG_LOG(SG_TERRAIN, SG_ALERT, "Coding error - AreaFeatureBin::LineFeature with fewer than three nodes"); 
        return; 
    }

    osg::Vec3d ma;

    // We're in Earth-centered coordinates, so "up" is simply directly away from (0,0,0)
    osg::Vec3d up = modelCenter;
    up.normalize();

    osg::ref_ptr<osgUtil::Tessellator> tessellator = new osgUtil::Tessellator;
    tessellator->setBoundaryOnly(false);
    tessellator->setTessellationNormal(up);
    tessellator->beginTessellation();
    tessellator->beginContour();

    // Build up the tesselator while also determining the correct elevation for the feature.
    double elev = 0;
    unsigned int elev_count = 0;
    osg::Vec3d last_pt;

    auto area_iter = area._nodes.begin();
    osg::Vec3d pt = *area_iter - modelCenter;
    ma = getMeshIntersection(buffer, masterLocator, pt, up);

    // Only build this area if the first vertex is on the mesh.  This ensures that the
    // area is only generated once, no matter how many tiles it spans.
    if (ma == pt) return;

    for (; area_iter != area._nodes.end(); area_iter++) {
        pt = *area_iter - modelCenter;

        // Ignore small segments - we really don't need resolution less than 10m
        if ((pt - last_pt).length2() < 100.0) continue;

        ma = getMeshIntersection(buffer, masterLocator, pt, up);
        if (ma !=pt) {
            elev += up*ma;
            elev_count++;
        }

        // To handle the case where the feature overlaps the edge of this particular mesh,
        // we always add vertices, even if they don't intersect the edge of the mesh.
        tessellator->addVertex(new osg::Vec3f(pt));

        last_pt = pt;
    }

    tessellator->endContour();
    tessellator->endTessellation();

    auto primList = tessellator->getPrimList();
    if (primList.size() == 0) return;

    unsigned int idx = 0;

    auto primItr = primList.begin();
    for (; primItr < primList.end(); ++ primItr) {
        auto vertices = (*primItr)->_vertices;
        std::for_each( vertices.begin(),
                        vertices.end(),
                        [v, t, n, up, elev, elev_count](auto vtx) {
                            v->push_back(*vtx + up * (elev / elev_count));
                            t->push_back(osg::Vec2f(vtx->x(), vtx->y()));
                            n->push_back(up);
                        }
                        );
        geometry->addPrimitiveSet(new osg::DrawArrays((*primItr)->_mode, idx, vertices.size()));
        idx += vertices.size();
    }
}

void VPBTechnique::applyCoastline(BufferData& buffer, Locator* masterLocator)
{
    unsigned int coast_features_lod_range = 4;
    float coastWidth = 40.0;

    const unsigned int tileLevel = _terrainTile->getTileID().level;
    const SGPropertyNode* propertyNode = _options->getPropertyNode().get();

    if (propertyNode) {
        const SGPropertyNode* static_lod = propertyNode->getNode("/sim/rendering/static-lod");
        coast_features_lod_range = static_lod->getIntValue("coastline-lod-level", coast_features_lod_range);
        coastWidth = static_lod->getFloatValue("coastline-width", coastWidth);
    }

    if (tileLevel < coast_features_lod_range) {
        // Do not generate coasts for tiles too far away
        return;
    }

    const SGMaterialLibPtr matlib  = _options->getMaterialLib();
    if (! matlib) {
        SG_LOG(SG_TERRAIN, SG_ALERT, "Unable to get materials library to generate areas");
        return;
    }    

    // Get all appropriate coasts.  We assume that the VPB terrain tile is smaller than a Bucket size.
    const osg::Vec3d world = buffer._transform->getMatrix().getTrans();
    const SGGeod loc = SGGeod::fromCart(toSG(world));
    const SGBucket bucket = SGBucket(loc);
    auto coasts = std::find_if(_coastFeatureLists.begin(), _coastFeatureLists.end(), [bucket](BucketCoastlineBinList b){return (b.first == bucket);});

    if (coasts == _coastFeatureLists.end()) return;

    // We're in Earth-centered coordinates, so "up" is simply directly away from (0,0,0)
    osg::Vec3d up = world;
    up.normalize();

    TileBounds tileBounds(masterLocator, up);

    SGMaterialCache* matcache = matlib->generateMatCache(loc, _options);
    SGMaterial* mat = matcache->find("ws30coastline");

    if (!mat) {
        SG_LOG(SG_TERRAIN, SG_ALERT, "Unable to find material ws30coastline at " << loc << " " << bucket);
        return;
    }    

    unsigned int xsize = mat->get_xsize();
    unsigned int ysize = mat->get_ysize();

    //  Generate a geometry for this set of coasts.
    osg::Vec3Array* v = new osg::Vec3Array;
    osg::Vec2Array* t = new osg::Vec2Array;
    osg::Vec3Array* n = new osg::Vec3Array;
    osg::Vec4Array* c = new osg::Vec4Array;

    for (; coasts != _coastFeatureLists.end(); ++coasts) {
        const CoastlineBinList coastBins = coasts->second;

        for (auto rb = coastBins.begin(); rb != coastBins.end(); ++rb)
        {
            auto coastFeatures = rb->getCoastlines();

            for (auto r = coastFeatures.begin(); r != coastFeatures.end(); ++r) {
                auto clipped = tileBounds.clipToTile(r->_nodes);
                if (clipped.size() > 1) {                    
                    // We need at least two points to render a line.
                    LineFeatureBin::LineFeature line = LineFeatureBin::LineFeature(clipped, coastWidth);
                    generateCoastlineFeature(buffer, masterLocator, line, world, v, t, n, xsize, ysize);
                }
            }
        }
    }

    if (v->size() == 0) return;

    c->push_back(osg::Vec4(1.0,1.0,1.0,1.0));

    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->setVertexArray(v);
    geometry->setTexCoordArray(0, t, osg::Array::BIND_PER_VERTEX);
    geometry->setTexCoordArray(1, t, osg::Array::BIND_PER_VERTEX);
    geometry->setNormalArray(n, osg::Array::BIND_PER_VERTEX);
    geometry->setColorArray(c, osg::Array::BIND_OVERALL);
    geometry->setUseDisplayList( false );
    geometry->setUseVertexBufferObjects( true );
    geometry->addPrimitiveSet( new osg::DrawArrays( GL_TRIANGLES, 0, v->size()) );

    EffectGeode* geode = new EffectGeode;
    geode->addDrawable(geometry);

    geode->setMaterial(mat);
    geode->setEffect(mat->get_one_effect(0));
    geode->setNodeMask(SG_NODEMASK_TERRAIN_BIT);
    buffer._transform->addChild(geode);
}

void VPBTechnique::generateCoastlineFeature(BufferData& buffer, Locator* masterLocator, LineFeatureBin::LineFeature coastline, osg::Vec3d modelCenter, osg::Vec3Array* v, osg::Vec2Array* t, osg::Vec3Array* n, unsigned int xsize, unsigned int ysize)
{
    if (coastline._nodes.size() < 2) { 
        SG_LOG(SG_TERRAIN, SG_ALERT, "Coding error - LineFeatureBin::LineFeature with fewer than two nodes"); 
        return; 
    }

    osg::Vec3d ma, mb;
    std::list<osg::Vec3d> coastlinePoints;
    auto coastline_iter = coastline._nodes.begin();

    // We're in Earth-centered coordinates, so "up" is simply directly away from (0,0,0)
    osg::Vec3d up = modelCenter;
    up.normalize();

    ma = getMeshIntersection(buffer, masterLocator, *coastline_iter - modelCenter, up);
    coastline_iter++;

    for (; coastline_iter != coastline._nodes.end(); coastline_iter++) {
        mb = getMeshIntersection(buffer, masterLocator, *coastline_iter - modelCenter, up);
        auto esl = VPBElevationSlice::computeVPBElevationSlice(buffer._landGeometry, ma, mb, up);

        for(auto eslitr = esl.begin(); eslitr != esl.end(); ++eslitr) {
            coastlinePoints.push_back(*eslitr);
        }

        // Now traverse the next segment
        ma = mb;
    }

    if (coastlinePoints.size() == 0) return;

    // We now have a series of points following the topography of the elevation mesh.

    auto iter = coastlinePoints.begin();
    osg::Vec3d start = *iter;
    iter++;

    osg::Vec3d last_spanwise =  (*iter - start)^ up;
    last_spanwise.normalize();

    float yTexBaseA = 0.0f;
    float yTexBaseB = 0.0f;

    for (; iter != coastlinePoints.end(); iter++) {

        osg::Vec3d end = *iter;

        // Ignore small segments - we really don't need resolution less than 10m
        if ((end - start).length2() < 100.0) continue;

        // Find a spanwise vector
        osg::Vec3d spanwise = ((end-start) ^ up);
        spanwise.normalize();

        // Define the coastline extents. Angle it down slightly on the seaward side (b->d).
        // OSM coastlines are always with the 
        const osg::Vec3d a = start + up;
        const osg::Vec3d b = start + last_spanwise * coastline._width;
        const osg::Vec3d c = end   + up;
        const osg::Vec3d d = end   + spanwise * coastline._width;

        // Determine the x and y texture coordinates for the edges
        const float xTex = coastline._width / xsize;
        const float yTexA = yTexBaseA + (c-a).length() / ysize;
        const float yTexB = yTexBaseB + (d-b).length() / ysize;

        // Now generate two triangles, .
        v->push_back(a);
        v->push_back(b);
        v->push_back(c);

        t->push_back(osg::Vec2d(0, yTexBaseA));
        t->push_back(osg::Vec2d(xTex, yTexBaseB));
        t->push_back(osg::Vec2d(0, yTexA));

        v->push_back(b);
        v->push_back(d);
        v->push_back(c);

        t->push_back(osg::Vec2d(xTex, yTexBaseB));
        t->push_back(osg::Vec2d(xTex, yTexB));
        t->push_back(osg::Vec2d(0, yTexA));

        // Normal is straight from the quad
        osg::Vec3d normal = -(end-start)^spanwise;
        normal.normalize();
        for (unsigned int i = 0; i < 6; i++) n->push_back(normal);

        start = end;
        yTexBaseA = yTexA;
        yTexBaseB = yTexB;
        last_spanwise = spanwise;
    }
}

// Find the intersection of a given SGGeod with the terrain mesh
osg::Vec3d VPBTechnique::getMeshIntersection(BufferData& buffer, Locator* masterLocator, osg::Vec3d pt, osg::Vec3d up) 
{
    osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector;
    intersector = new osgUtil::LineSegmentIntersector(pt - up*100.0, pt + up*8000.0);
    osgUtil::IntersectionVisitor visitor(intersector.get());
    buffer._landGeometry->accept(visitor);

    if (intersector->containsIntersections()) {
        // We have an intersection with the terrain model, so return it
        return intersector->getFirstIntersection().getWorldIntersectPoint();
    } else {
        // No intersection.  Likely this point is outside our geometry.  So just return the original element.
        return pt;
    }
}

void VPBTechnique::update(osg::NodeVisitor& nv)
{
    if (_terrainTile) _terrainTile->osg::Group::traverse(nv);

    if (_newBufferData.valid())
    {
        _currentBufferData = _newBufferData;
        _newBufferData = 0;
    }
}


void VPBTechnique::cull(osg::NodeVisitor& nv)
{
    if (_currentBufferData.valid())
    {
        if (_currentBufferData->_transform.valid())
        {
            _currentBufferData->_transform->accept(nv);
        }
    }
}


void VPBTechnique::traverse(osg::NodeVisitor& nv)
{
    if (!_terrainTile) return;

    // if app traversal update the frame count.
    if (nv.getVisitorType()==osg::NodeVisitor::UPDATE_VISITOR)
    {
        //if (_terrainTile->getDirty()) _terrainTile->init(_terrainTile->getDirtyMask(), false);
        update(nv);
        return;
    }
    else if (nv.getVisitorType()==osg::NodeVisitor::CULL_VISITOR)
    {
        cull(nv);
        return;
    }


    if (_terrainTile->getDirty())
    {
        OSG_INFO<<"******* Doing init ***********"<<std::endl;
        //_terrainTile->init(_terrainTile->getDirtyMask(), false);
    }

    if (_currentBufferData.valid())
    {
        if (_currentBufferData->_transform.valid()) _currentBufferData->_transform->accept(nv);
    }
}

void VPBTechnique::cleanSceneGraph()
{
}

void VPBTechnique::releaseGLObjects(osg::State* state) const
{
    if (_currentBufferData.valid() && _currentBufferData->_transform.valid()) _currentBufferData->_transform->releaseGLObjects(state);
    if (_newBufferData.valid() && _newBufferData->_transform.valid()) _newBufferData->_transform->releaseGLObjects(state);
}

// Add an osg object representing an elevation contraint on the terrain mesh.  The generated terrain mesh will not include any vertices that
// lie above the constraint model.  (Note that geometry may result in edges intersecting the constraint model in cases where there
// are significantly higher vertices that lie just outside the constraint model.
void VPBTechnique::addElevationConstraint(osg::ref_ptr<osg::Node> constraint)
{ 
    const std::lock_guard<std::mutex> lock(VPBTechnique::_elevationConstraintMutex); // Lock the _elevationConstraintGroup for this scope
    _elevationConstraintGroup->addChild(constraint.get()); 
}

// Remove a previously added constraint.  E.g on model unload.
void VPBTechnique::removeElevationConstraint(osg::ref_ptr<osg::Node> constraint)
{ 
    const std::lock_guard<std::mutex> lock(VPBTechnique::_elevationConstraintMutex); // Lock the _elevationConstraintGroup for this scope
    _elevationConstraintGroup->removeChild(constraint.get()); 
}

// Check a given vertex against any elevation constraints  E.g. to ensure the terrain mesh doesn't
// poke through any airport meshes.  If such a constraint exists, the function will return a replacement
// vertex displaces such that it lies 1m below the contraint relative to the passed in origin.  
osg::Vec3d VPBTechnique::checkAgainstElevationConstraints(osg::Vec3d origin, osg::Vec3d vertex, float vtx_gap)
{
    const std::lock_guard<std::mutex> lock(VPBTechnique::_elevationConstraintMutex); // Lock the _elevationConstraintGroup for this scope
    osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector;
    intersector = new osgUtil::LineSegmentIntersector(origin, vertex);
    osgUtil::IntersectionVisitor visitor(intersector.get());
    _elevationConstraintGroup->accept(visitor);

    if (intersector->containsIntersections()) {
        // We have an intersection with our constraints model, so move the terrain vertex to 1m below the intersection point
        osg::Vec3d ray = intersector->getFirstIntersection().getWorldIntersectPoint() - origin;
        ray.normalize();
        return intersector->getFirstIntersection().getWorldIntersectPoint() - ray*vtx_gap;
    } else {
        return vertex;
    }
}

// Add an osg object representing a contraint on the terrain mesh. The generated
// terrain mesh will not include any random objects intersecting with the
// constraint model.
void VPBTechnique::addRandomObjectsConstraint(osg::ref_ptr<osg::Node> constraint)
{ 
    _randomObjectsConstraintGroup->addChild(constraint.get());
}

// Remove a previously added constraint.  E.g on model unload.
void VPBTechnique::removeRandomObjectsConstraint(osg::ref_ptr<osg::Node> constraint)
{ 
    _randomObjectsConstraintGroup->removeChild(constraint.get());
}

// Remove all the constraints, which will still be referenced by the terrain tile itself.
void VPBTechnique::clearRandomObjectsConstraints()
{ 
    _randomObjectsConstraintGroup->removeChildren(0, _randomObjectsConstraintGroup->getNumChildren());
}

void VPBTechnique::clearConstraints()
{
    const std::lock_guard<std::mutex> elock(VPBTechnique::_elevationConstraintMutex); // Lock the _elevationConstraintGroup for this scope
    _elevationConstraintGroup = new osg::Group();
}

void VPBTechnique::addLineFeatureList(SGBucket bucket, LineFeatureBinList roadList)
{
    if (roadList.empty()) return;

    // Block to mutex the List alone
    {
        const std::lock_guard<std::mutex> lock(VPBTechnique::_lineFeatureLists_mutex); // Lock the _lineFeatureLists for this scope
        _lineFeatureLists.push_back(std::pair(bucket, roadList));
    }

}

void VPBTechnique::addAreaFeatureList(SGBucket bucket, AreaFeatureBinList areaList)
{
    if (areaList.empty()) return;

    // Block to mutex the List alone
    {
        const std::lock_guard<std::mutex> lock(VPBTechnique::_areaFeatureLists_mutex); // Lock the _lineFeatureLists for this scope
        _areaFeatureLists.push_back(std::pair(bucket, areaList));
    }
}

void VPBTechnique::addCoastlineList(SGBucket bucket, CoastlineBinList coastline)
{
    if (coastline.empty()) return;

    // Block to mutex the List alone
    {
        const std::lock_guard<std::mutex> lock(VPBTechnique::_coastFeatureLists_mutex); // Lock the _lineFeatureLists for this scope
        _coastFeatureLists.push_back(std::pair(bucket, coastline));
    }

}

void VPBTechnique::unloadFeatures(SGBucket bucket)
{
    SG_LOG(SG_TERRAIN, SG_DEBUG, "Erasing all features with entry " << bucket);
    const std::lock_guard<std::mutex> lock(VPBTechnique::_lineFeatureLists_mutex); // Lock the _lineFeatureLists for this scope
    // C++ 20...
    //std::erase_if(_lineFeatureLists, [bucket](BucketLineFeatureBinList p) { return p.first == bucket; } );
    //std::erase_if(_lineFeatureLists, [bucket](BucketAreaFeatureBinList p) { return p.first == bucket; } );
    //std::erase_if(_coastFeatureLists, [bucket](BucketCoastlineBinList p) { return p.first == bucket; } );
}


