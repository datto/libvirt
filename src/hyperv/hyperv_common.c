#include <config.h>

#include "viralloc.h"
#include "hyperv_common.h"

void
hypervFreePrivate(hypervPrivate **priv)
{
    if (priv == NULL || *priv == NULL)
        return;

    if ((*priv)->client != NULL) {
        /* FIXME: This leaks memory due to bugs in openwsman <= 2.2.6 */
        wsmc_release((*priv)->client);
    }

    hypervFreeParsedUri(&(*priv)->parsedUri);
    VIR_FREE(*priv);
}
