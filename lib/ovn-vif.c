#include <config.h>

#include "plug-provider.h"

extern const struct plug_class plug_representor;

const struct plug_class *plug_provider_classes[] = {
    &plug_representor,
    NULL,
};
