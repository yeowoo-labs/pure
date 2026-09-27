#ifndef PURE_MODULE_STORE_H
#define PURE_MODULE_STORE_H
#include "module.h"
/* Keyed module collections and the app's states on the selected tree store:
 * one event opens the message key as a held draft and commits it together
 * with the states the event changed. The path outlives the app. */
PureData pure_tree_data(const char *path);
/* The same through a store handle (state.h): opening a document or state
 * checks only whether the store changed. The handle outlives the app. */
PureData pure_store_data(PureStore *);
#endif
