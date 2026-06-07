/*
 * ct_file.h - ChainTree Binary Image File Layer
 *
 * Reads a .ctb file from disk and calls ct_image_load().
 * Also supports loading from an embedded const uint8_t[] array
 * (the _image.h output from the pipeline).
 *
 * Usage (file):
 *   ct_image_t img;
 *   int rc = ct_file_load("my_chaintree.ctb", &img);
 *   // register functions...
 *   ct_file_unload(&img);
 *
 * Usage (embedded):
 *   #include "my_chaintree_image.h"
 *   ct_image_t img;
 *   int rc = ct_embedded_load(my_chaintree_image, MY_CHAINTREE_IMAGE_SIZE, &img);
 *   // register functions...
 *   ct_image_free(&img);  // no file buffer to free
 */

#ifndef CFL_FILE_LOADER_H
#define CFL_FILE_LOADER_H

#include "cfl_image_loader.h"

/* Additional error codes for file operations */
#define CFL_FILE_LOADER_ERR_FILE_OPEN    -20
#define CFL_FILE_LOADER_ERR_FILE_READ    -21
#define CFL_FILE_LOADER_ERR_FILE_SIZE    -22

/*
 * Load a .ctb binary image from a file path.
 *
 * Reads the entire file into a malloc'd buffer, then calls ct_image_load().
 * The buffer is stored internally and freed by ct_file_unload().
 *
 * path: filesystem path to .ctb file
 * out:  pointer to caller-allocated ct_image_t
 *
 * Returns CT_OK on success, negative error code on failure.
 */
int cfl_file_loader_load(const char *path, cfl_image_loader_t *out);

/*
 * Unload a file-loaded image.
 *
 * Calls ct_image_free() and also frees the file buffer.
 * Only use this for images loaded via ct_file_load().
 * For embedded images, use ct_image_free() directly.
 */
void cfl_file_loader_unload(cfl_image_loader_t *img);

/*
 * Load from an embedded const uint8_t[] array (the _image.h output).
 *
 * Zero-copy: the image data is used in place, no allocation for the
 * image buffer itself. The array must remain valid for the lifetime
 * of the ct_image_t.
 *
 * data: pointer to const uint8_t array
 * size: size in bytes (use the _IMAGE_SIZE define)
 * out:  pointer to caller-allocated ct_image_t
 *
 * Returns CT_OK on success, negative error code on failure.
 */
int cfl_file_loader_embedded_load(const uint8_t *data, uint32_t size, cfl_image_loader_t *out);

#endif /* CT_FILE_H */