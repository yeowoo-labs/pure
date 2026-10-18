#ifndef PURE_MODULE_STORE_H
#define PURE_MODULE_STORE_H
#include "module.h"
/* The app's states on the selected tree store, each save one commit. The path outlives the app. */
PureData pure_tree_data(const char *path);
/* The same through a store handle (state.h): opening a state checks only whether the store changed. The handle outlives
 * the app. */
PureData pure_store_data(PureStore *);
#endif
