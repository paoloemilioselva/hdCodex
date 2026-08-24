#include "instancer.h"

#include "render_param.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

template <class Array, class Output>
bool ReadElement(const VtValue& value, int index, Output* output)
{
    if (!value.IsHolding<Array>() || index < 0) return false;
    const Array& array = value.UncheckedGet<Array>();
    if (static_cast<std::size_t>(index) >= array.size()) return false;
    *output = Output(array[static_cast<std::size_t>(index)]);
    return true;
}

bool ReadVec3(const VtValue& value, int index, GfVec3d* output)
{
    return ReadElement<VtVec3fArray>(value, index, output) ||
        ReadElement<VtVec3dArray>(value, index, output);
}

bool ReadRotation(const VtValue& value, int index, GfQuatd* output)
{
    if (ReadElement<VtQuatfArray>(value, index, output) ||
        ReadElement<VtQuatdArray>(value, index, output) ||
        ReadElement<VtQuathArray>(value, index, output)) return true;
    GfVec4f vector;
    if (!ReadElement<VtVec4fArray>(value, index, &vector)) return false;
    *output = GfQuatd(vector[0], vector[1], vector[2], vector[3]);
    return true;
}

bool ReadMatrix(const VtValue& value, int index, GfMatrix4d* output)
{
    return ReadElement<VtMatrix4dArray>(value, index, output) ||
        ReadElement<VtMatrix4fArray>(value, index, output);
}

} // namespace

HdCodexInstancer::HdCodexInstancer(HdSceneDelegate* delegate, const SdfPath& id)
    : HdInstancer(delegate, id)
{
}

HdCodexInstancer::~HdCodexInstancer() = default;

void HdCodexInstancer::Sync(HdSceneDelegate* sceneDelegate,
                            HdRenderParam* renderParam,
                            HdDirtyBits* dirtyBits)
{
    bool changed = false;
    {
        const std::scoped_lock lock(_mutex);
        if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
            _visible = sceneDelegate->GetVisible(GetId());
            changed = true;
        }
        _UpdateInstancer(sceneDelegate, dirtyBits);
        if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
            _primvars.clear();
            const HdPrimvarDescriptorVector descriptors =
                sceneDelegate->GetPrimvarDescriptors(
                    GetId(), HdInterpolationInstance);
            for (const HdPrimvarDescriptor& descriptor : descriptors) {
                VtValue value = sceneDelegate->Get(GetId(), descriptor.name);
                if (!value.IsEmpty()) _primvars[descriptor.name] = std::move(value);
            }
            changed = true;
        }
    }
    if (changed) {
        if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
            param->MarkSceneDirty();
        }
    }
    *dirtyBits = HdChangeTracker::Clean;
}

VtMatrix4dArray HdCodexInstancer::ComputeInstanceTransforms(
    const SdfPath& prototypeId) const
{
    VtIntArray instanceIndices =
        GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    GfMatrix4d instancerTransform =
        GetDelegate()->GetInstancerTransform(GetId());
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor> primvars;
    {
        const std::scoped_lock lock(_mutex);
        if (!_visible) return {};
        primvars = _primvars;
    }

    VtMatrix4dArray transforms(instanceIndices.size(), instancerTransform);
    const auto apply = [&](const TfToken& name, const auto& operation) {
        const auto found = primvars.find(name);
        if (found == primvars.end()) return;
        for (std::size_t item = 0; item < instanceIndices.size(); ++item) {
            operation(found->second, instanceIndices[item], transforms[item]);
        }
    };

    apply(HdInstancerTokens->instanceTranslations,
        [](const VtValue& value, int index, GfMatrix4d& transform) {
            GfVec3d translation;
            if (ReadVec3(value, index, &translation)) {
                GfMatrix4d matrix(1.0);
                matrix.SetTranslate(translation);
                transform = matrix * transform;
            }
        });
    apply(HdInstancerTokens->instanceRotations,
        [](const VtValue& value, int index, GfMatrix4d& transform) {
            GfQuatd rotation;
            if (ReadRotation(value, index, &rotation)) {
                GfMatrix4d matrix(1.0);
                matrix.SetRotate(rotation);
                transform = matrix * transform;
            }
        });
    apply(HdInstancerTokens->instanceScales,
        [](const VtValue& value, int index, GfMatrix4d& transform) {
            GfVec3d scale;
            if (ReadVec3(value, index, &scale)) {
                GfMatrix4d matrix(1.0);
                matrix.SetScale(scale);
                transform = matrix * transform;
            }
        });
    apply(HdInstancerTokens->instanceTransforms,
        [](const VtValue& value, int index, GfMatrix4d& transform) {
            GfMatrix4d matrix;
            if (ReadMatrix(value, index, &matrix)) transform = matrix * transform;
        });

    if (GetParentId().IsEmpty()) return transforms;
    HdInstancer* parent =
        GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());
    auto* codexParent = dynamic_cast<HdCodexInstancer*>(parent);
    if (!TF_VERIFY(codexParent)) return transforms;
    const VtMatrix4dArray parentTransforms =
        codexParent->ComputeInstanceTransforms(GetId());
    VtMatrix4dArray flattened(parentTransforms.size() * transforms.size());
    for (std::size_t parentIndex = 0; parentIndex < parentTransforms.size(); ++parentIndex) {
        for (std::size_t localIndex = 0; localIndex < transforms.size(); ++localIndex) {
            flattened[parentIndex * transforms.size() + localIndex] =
                transforms[localIndex] * parentTransforms[parentIndex];
        }
    }
    return flattened;
}

PXR_NAMESPACE_CLOSE_SCOPE
