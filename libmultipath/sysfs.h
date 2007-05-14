/*
 * sysfs.h
 */

#ifndef _LIBMULTIPATH_SYSFS_H
#define _LIBMULTIPATH_SYSFS_H

#define dbg(format, arg...) do {} while (0)

int sysfs_init(char *path, size_t len);
void sysfs_cleanup(void);
void sysfs_device_set_values(struct sysfs_device *dev, const char *devpath,
			     const char *subsystem, const char *driver);
struct sysfs_device *sysfs_device_get(const char *devpath);
struct sysfs_device *sysfs_device_get_parent(struct sysfs_device *dev);
struct sysfs_device *sysfs_device_get_parent_with_subsystem(struct sysfs_device *dev, const char *subsystem);
char *sysfs_attr_get_value(const char *devpath, const char *attr_name);
int sysfs_resolve_link(char *path, size_t size);

#endif
