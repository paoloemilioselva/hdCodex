#include "subdivision.h"

#include "pxr/imaging/pxOsd/tokens.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

HdMeshTopology CubeTopology()
{
    return HdMeshTopology(
        PxOsdOpenSubdivTokens->catmullClark,
        PxOsdOpenSubdivTokens->rightHanded,
        VtIntArray{4, 4, 4, 4, 4, 4},
        VtIntArray{
            0, 1, 2, 3,
            4, 7, 6, 5,
            0, 4, 5, 1,
            1, 5, 6, 2,
            2, 6, 7, 3,
            4, 0, 3, 7});
}

VtVec3fArray CubePoints()
{
    return {
        {-1.0F, -1.0F, -1.0F}, {1.0F, -1.0F, -1.0F},
        {1.0F, 1.0F, -1.0F}, {-1.0F, 1.0F, -1.0F},
        {-1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, 1.0F},
        {1.0F, 1.0F, 1.0F}, {-1.0F, 1.0F, 1.0F}};
}

void TestCatmullClarkCountsAndMapping()
{
    HdCodexRefinedMeshGeometry result;
    std::string error;
    Check(HdCodexRefineMesh(
        CubeTopology(), CubePoints(), {}, {}, HdInterpolationConstant,
        1, &result, &error), error.c_str());
    Check(result.points.size() == 26, "level-1 cube must have 26 vertices");
    Check(result.normals.size() == result.points.size(),
          "limit normals must match refined vertices");
    Check(result.topology.GetFaceVertexCounts().size() == 24,
          "level-1 cube must have 24 faces");
    Check(result.topology.GetFaceVertexIndices().size() == 96,
          "level-1 cube must have 96 face corners");
    Check(result.coarseFaceIndices.size() == 24,
          "every refined face must map to a coarse face");
    for (int coarseFace = 0; coarseFace < 6; ++coarseFace) {
        int count = 0;
        for (const int mapped : result.coarseFaceIndices) {
            if (mapped == coarseFace) ++count;
        }
        Check(count == 4, "each coarse cube face must produce four children");
    }
    for (const GfVec3f& normal : result.normals) {
        Check(std::isfinite(normal[0]) && std::isfinite(normal[1]) &&
                  std::isfinite(normal[2]),
              "limit normals must be finite");
        Check(std::abs(normal.GetLength() - 1.0F) < 1e-4F,
              "closed smooth cube limit normals must be normalized");
    }
}

void TestFaceVaryingCoordinates()
{
    VtVec2fArray coordinates;
    coordinates.reserve(24);
    for (int face = 0; face < 6; ++face) {
        coordinates.push_back({0.0F, 0.0F});
        coordinates.push_back({1.0F, 0.0F});
        coordinates.push_back({1.0F, 1.0F});
        coordinates.push_back({0.0F, 1.0F});
    }
    HdCodexRefinedMeshGeometry result;
    std::string error;
    Check(HdCodexRefineMesh(
        CubeTopology(), CubePoints(), coordinates, {},
        HdInterpolationFaceVarying, 1, &result, &error), error.c_str());
    Check(result.texcoordInterpolation == HdInterpolationFaceVarying,
          "face-varying interpolation must be preserved");
    Check(result.texcoords.size() == 96,
          "refined face-varying values must match refined face corners");
    for (const GfVec2f& coordinate : result.texcoords) {
        Check(std::isfinite(coordinate[0]) && std::isfinite(coordinate[1]),
              "refined texture coordinates must be finite");
        Check(coordinate[0] >= 0.0F && coordinate[0] <= 1.0F &&
              coordinate[1] >= 0.0F && coordinate[1] <= 1.0F,
              "refined texture coordinates left their source range");
    }
}

void TestIndexedFaceVaryingCoordinates()
{
    const VtVec2fArray coordinates{
        {0.0F, 0.0F}, {1.0F, 0.0F},
        {1.0F, 1.0F}, {0.0F, 1.0F}};
    VtIntArray indices;
    for (int face = 0; face < 6; ++face) {
        indices.push_back(0);
        indices.push_back(1);
        indices.push_back(2);
        indices.push_back(3);
    }
    HdCodexRefinedMeshGeometry result;
    std::string error;
    Check(HdCodexRefineMesh(
        CubeTopology(), CubePoints(), coordinates, indices,
        HdInterpolationFaceVarying, 1, &result, &error), error.c_str());
    Check(result.texcoords.size() == 96,
          "indexed face-varying output must match refined face corners");
    bool foundInteriorValue = false;
    for (const GfVec2f& coordinate : result.texcoords) {
        foundInteriorValue = foundInteriorValue ||
            (coordinate[0] > 0.0F && coordinate[0] < 1.0F) ||
            (coordinate[1] > 0.0F && coordinate[1] < 1.0F);
    }
    Check(foundInteriorValue,
          "indexed face-varying values were not interpolated");
}

void TestLoopAndBilinearSchemes()
{
    const VtVec3fArray tetrahedron{
        {1.0F, 1.0F, 1.0F}, {-1.0F, -1.0F, 1.0F},
        {-1.0F, 1.0F, -1.0F}, {1.0F, -1.0F, -1.0F}};
    const HdMeshTopology loop(
        PxOsdOpenSubdivTokens->loop,
        PxOsdOpenSubdivTokens->rightHanded,
        VtIntArray{3, 3, 3, 3},
        VtIntArray{0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2});
    HdCodexRefinedMeshGeometry loopResult;
    std::string error;
    Check(HdCodexRefineMesh(
        loop, tetrahedron, {}, {}, HdInterpolationConstant,
        1, &loopResult, &error), error.c_str());
    Check(loopResult.points.size() == 10,
          "level-1 Loop tetrahedron must have 10 vertices");
    Check(loopResult.topology.GetFaceVertexCounts().size() == 16,
          "level-1 Loop tetrahedron must have 16 faces");

    const HdMeshTopology bilinear(
        PxOsdOpenSubdivTokens->bilinear,
        PxOsdOpenSubdivTokens->leftHanded,
        VtIntArray{4}, VtIntArray{0, 1, 2, 3});
    const VtVec3fArray plane{
        {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
        {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    HdCodexRefinedMeshGeometry bilinearResult;
    Check(HdCodexRefineMesh(
        bilinear, plane, {}, {}, HdInterpolationConstant,
        2, &bilinearResult, &error), error.c_str());
    Check(bilinearResult.points.size() == 25,
          "level-2 bilinear quad must have 25 vertices");
    Check(bilinearResult.topology.GetFaceVertexCounts().size() == 16,
          "level-2 bilinear quad must have 16 faces");
    Check(bilinearResult.topology.GetOrientation() ==
              PxOsdOpenSubdivTokens->leftHanded,
          "refinement must preserve orientation");
}

void TestUniformCoordinatesFollowCoarseFaces()
{
    const VtVec2fArray coordinates{
        {0.0F, 0.0F}, {1.0F, 0.0F}, {2.0F, 0.0F},
        {3.0F, 0.0F}, {4.0F, 0.0F}, {5.0F, 0.0F}};
    HdCodexRefinedMeshGeometry result;
    std::string error;
    Check(HdCodexRefineMesh(
        CubeTopology(), CubePoints(), coordinates, {},
        HdInterpolationUniform, 1, &result, &error), error.c_str());
    Check(result.texcoords.size() == 24,
          "uniform values must be expanded to refined faces");
    for (std::size_t face = 0; face < result.texcoords.size(); ++face) {
        Check(result.texcoords[face][0] ==
                  static_cast<float>(result.coarseFaceIndices[face]),
              "uniform value did not follow its coarse face");
    }
}

void TestCreasesAndCorners()
{
    HdMeshTopology smoothTopology = CubeTopology();
    HdMeshTopology sharpTopology = CubeTopology();
    PxOsdSubdivTags tags = sharpTopology.GetSubdivTags();
    tags.SetCreaseIndices(VtIntArray{
        0, 1, 1, 2, 2, 3, 3, 0,
        4, 5, 5, 6, 6, 7, 7, 4,
        0, 4, 1, 5, 2, 6, 3, 7});
    tags.SetCreaseLengths(VtIntArray(12, 2));
    tags.SetCreaseWeights(VtFloatArray(12, 10.0F));
    tags.SetCornerIndices(VtIntArray{0});
    tags.SetCornerWeights(VtFloatArray{10.0F});
    sharpTopology.SetSubdivTags(tags);

    HdCodexRefinedMeshGeometry smooth;
    HdCodexRefinedMeshGeometry sharp;
    std::string error;
    Check(HdCodexRefineMesh(
        smoothTopology, CubePoints(), {}, {}, HdInterpolationConstant,
        1, &smooth, &error), error.c_str());
    Check(HdCodexRefineMesh(
        sharpTopology, CubePoints(), {}, {}, HdInterpolationConstant,
        1, &sharp, &error), error.c_str());

    float smoothExtent = 0.0F;
    float sharpExtent = 0.0F;
    for (const GfVec3f& point : smooth.points) {
        smoothExtent = std::max(smoothExtent,
            std::max(std::abs(point[0]),
                std::max(std::abs(point[1]), std::abs(point[2]))));
    }
    for (const GfVec3f& point : sharp.points) {
        sharpExtent = std::max(sharpExtent,
            std::max(std::abs(point[0]),
                std::max(std::abs(point[1]), std::abs(point[2]))));
    }
    Check(sharpExtent > smoothExtent + 0.1F,
          "crease/corner tags did not preserve the sharp cube extent");
    Check(std::abs(sharpExtent - 1.0F) < 1e-4F,
          "infinitely sharp cube moved away from its control cage");
}

void TestHoleRemoval()
{
    HdMeshTopology topology(
        PxOsdOpenSubdivTokens->catmullClark,
        PxOsdOpenSubdivTokens->rightHanded,
        VtIntArray{4, 4},
        VtIntArray{0, 1, 4, 3, 1, 2, 5, 4},
        VtIntArray{1});
    const VtVec3fArray points{
        {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
        {2.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
        {1.0F, 1.0F, 0.0F}, {2.0F, 1.0F, 0.0F}};
    HdCodexRefinedMeshGeometry result;
    std::string error;
    Check(HdCodexRefineMesh(
        topology, points, {}, {}, HdInterpolationConstant,
        1, &result, &error), error.c_str());
    Check(result.topology.GetFaceVertexCounts().size() == 4,
          "children of a coarse hole must be omitted");
    for (const int coarseFace : result.coarseFaceIndices) {
        Check(coarseFace == 0, "a retained face mapped back to the coarse hole");
    }
}

} // namespace

int main()
{
    TestCatmullClarkCountsAndMapping();
    TestFaceVaryingCoordinates();
    TestIndexedFaceVaryingCoordinates();
    TestLoopAndBilinearSchemes();
    TestUniformCoordinatesFollowCoarseFaces();
    TestCreasesAndCorners();
    TestHoleRemoval();
    std::cout << "subdivision tests passed\n";
    return EXIT_SUCCESS;
}
