/*
 * Placeholder translation unit (external-plugin-runtime plan U1): the
 * GinkgoPluginABI target ships only the header — plugins provide the
 * entry symbol — but SwiftPM builds a C target from at least one source
 * file, so this one exists to compile. It intentionally defines nothing.
 */
#include "ginkgo_plugin.h"
