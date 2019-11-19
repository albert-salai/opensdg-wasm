#ifndef INTERNAL_REGISTRY_H
#define INTERNAL_REGISTRY_H

void registry_add_connection(struct _osdg_connection *conn);
void registry_remove_connection(struct _osdg_connection *conn);
struct _osdg_connection *registry_find_connection(int uid);

#endif
