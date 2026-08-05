#pragma once

#include "ofxsImageEffect.h"

#define kPluginName        "Multi Transform"
#define kPluginGrouping    "MultiTransform"
#define kPluginDescription "Multi-stage animated transform with staggered timing and motion blur."
#define kPluginIdentifier  "com.resolvemultitransform.MultiTransform"
#define kPluginVersionMajor 0
#define kPluginVersionMinor 1

class MultiTransformPluginFactory : public OFX::PluginFactoryHelper<MultiTransformPluginFactory>
{
public:
    MultiTransformPluginFactory();

    virtual void describe(OFX::ImageEffectDescriptor& p_Desc);
    virtual void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context);
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context);
};
