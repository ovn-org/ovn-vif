#include <config.h>

#include "vif-plug-provider.h"

extern const struct vif_plug_class vif_plug_representor;

const struct vif_plug_class *vif_plug_provider_classes[] = {
    &vif_plug_representor,
    NULL,
};
