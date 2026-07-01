/* Shim so consumers' `#include <yajl/yajl_parse.h>` resolves to the verbatim
 * upstream header kept under api/. See README.json2toon. */
#include "../api/yajl_parse.h"
