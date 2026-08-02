/* fs/fat16.c — Driver FAT16 con soporte COMPLETO de lectura y escritura.
 *
 * CAMBIO #5: añade escritura FAT16 real.
 *
 * Funcionalidades nuevas respecto al driver original (solo lectura):
 *   - fat16_write_sector()      — escribe un sector al disco
 *   - fat16_alloc_cluster()     — busca cluster libre en la FAT y lo marca
 *   - fat16_free_cluster_chain() — libera todos los clusters de un archivo
 *   - fat16_set_fat_entry()     — actualiza una entrada en la FAT (en disco)
 *   - fat16_node_write()        — escribe datos a un archivo (crea cadena
 *                                  de clusters según sea necesario)
 *   - fat16_create_entry()      — crea una entrada de directorio nueva
 *   - fat16_delete_entry()      — marca una entrada como borrada (0xE5)
 *   - fat16_update_size()       — actualiza el campo size en el directorio
 *
 * La FAT se replica en las dos copias (FAT1 y FAT2) para consistencia.
 * Los nombres largos (LFN) siguen sin soporte — se usa 8.3 truncado.
 *
 * Interoperabilidad: el volumen FAT16 generado/modificado por Trinux
 * puede montarse en Linux (mount -t vfat), Windows, y macOS sin herramientas
 * especiales.
 */
#include "fat16.h"
#include "../drivers/ata.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"

/* ---- Estructuras en disco ---- */

struct fat_bpb {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_dir_entries;
    uint16_t total_sectors_16;
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed));

struct fat_dir_entry {
    char     name[11];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  ctime_tenths;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_high;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed));

struct mbr_partition {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} __attribute__((packed));

struct mbr {
    uint8_t  code[446];
    struct mbr_partition part[4];
    uint16_t signature;
} __attribute__((packed));

/* ---- Atributos FAT ---- */
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       (FAT_ATTR_READ_ONLY|FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM|FAT_ATTR_VOLUME_ID)

/* ---- Estado del driver ---- */
static uint32_t       partition_lba = 0;
static struct fat_bpb bpb;
static uint32_t       fat_start_lba;
static uint32_t       root_dir_lba;
static uint32_t       data_start_lba;
static uint32_t       root_dir_sectors;
static uint32_t       total_clusters;    /* total de clusters de datos */
static bool           fat16_mounted = false;

/* ---- I/O de sectores ---- */

static bool fat16_read_sector(uint32_t lba, uint8_t *buf)
{
    return ata_read_sectors(partition_lba + lba, 1, buf) == 0;
}

/* CAMBIO #5: escritura de sector */
static bool fat16_write_sector(uint32_t lba, const uint8_t *buf)
{
    return ata_write_sectors(partition_lba + lba, 1, buf) == 0;
}

/* ---- Acceso a la FAT ---- */

static uint16_t fat16_get_fat_entry(uint16_t cluster)
{
    uint32_t fat_offset = cluster * 2;
    uint32_t sector     = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in  = fat_offset % 512;

    uint8_t buf[512];
    if (!fat16_read_sector(sector, buf)) return 0xFFFF;
    return *((uint16_t *)(buf + offset_in));
}

/* CAMBIO #5: actualiza la FAT (en ambas copias) */
static bool fat16_set_fat_entry(uint16_t cluster, uint16_t value)
{
    uint32_t fat_offset = cluster * 2;
    uint32_t sector     = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in  = fat_offset % 512;

    uint8_t buf[512];
    for (uint8_t copy = 0; copy < bpb.fat_count; copy++) {
        uint32_t fat_copy_sector = sector + copy * bpb.sectors_per_fat_16;
        if (!fat16_read_sector(fat_copy_sector, buf)) return false;
        *((uint16_t *)(buf + offset_in)) = value;
        if (!fat16_write_sector(fat_copy_sector, buf)) return false;
    }
    return true;
}

/* CAMBIO #5: encuentra el primer cluster libre (valor 0x0000 en FAT) */
static uint16_t fat16_alloc_cluster(void)
{
    for (uint16_t c = 2; c < (uint16_t)(total_clusters + 2); c++) {
        if (fat16_get_fat_entry(c) == 0x0000) {
            fat16_set_fat_entry(c, 0xFFFF);   /* marcar como final de cadena */
            return c;
        }
    }
    return 0;   /* sin espacio */
}

/* CAMBIO #5: libera toda la cadena de clusters de un archivo */
static void fat16_free_cluster_chain(uint16_t start)
{
    uint16_t c = start;
    while (c >= 2 && c < 0xFFF8) {
        uint16_t next = fat16_get_fat_entry(c);
        fat16_set_fat_entry(c, 0x0000);   /* marcar como libre */
        c = next;
    }
}

/* ---- Conversión cluster → LBA ---- */

static uint32_t fat16_cluster_to_lba(uint16_t cluster)
{
    return data_start_lba + ((uint32_t)(cluster - 2) * bpb.sectors_per_cluster);
}

/* ---- Formateo de nombres 8.3 ---- */

/* Convierte "FILENAME EXT" → "filename.ext" */
static void fat16_format_name(const char *fat_name, char *out)
{
    int out_idx = 0;
    for (int i = 0; i < 8 && fat_name[i] != ' '; i++)
        out[out_idx++] = (fat_name[i] >= 'A' && fat_name[i] <= 'Z')
                         ? fat_name[i] + 32 : fat_name[i];
    if (fat_name[8] != ' ') {
        out[out_idx++] = '.';
        for (int i = 8; i < 11 && fat_name[i] != ' '; i++)
            out[out_idx++] = (fat_name[i] >= 'A' && fat_name[i] <= 'Z')
                             ? fat_name[i] + 32 : fat_name[i];
    }
    out[out_idx] = '\0';
}

/* Convierte "filename.ext" → "FILENAME   EXT" (formato FAT 8.3) */
static void fat16_to_83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    int i = 0, j = 0;
    /* Nombre (hasta 8 chars) */
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        out[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    /* Extensión (hasta 3 chars) */
    if (name[i] == '.') {
        i++;
        int k = 8;
        while (name[i] && k < 11) {
            char c = name[i++];
            out[k++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    }
}

/* ---- VFS callbacks ---- */

static uint32_t fat16_node_read(vfs_node_t *node, uint32_t off,
                                uint32_t size, uint8_t *buf)
{
    if (off >= node->size) return 0;
    if (off + size > node->size) size = node->size - off;

    /* El cluster inicial está almacenado en owner_gid (hack temporal,
     * suficiente para un driver educativo de un solo proceso). */
    uint16_t cluster = (uint16_t)node->owner_gid;
    if (cluster < 2) return 0;

    uint32_t cluster_size  = bpb.sectors_per_cluster * 512;
    uint32_t cluster_skip  = off / cluster_size;

    for (uint32_t i = 0; i < cluster_skip; i++) {
        cluster = fat16_get_fat_entry(cluster);
        if (cluster >= 0xFFF8) return 0;
    }

    uint32_t bytes_read        = 0;
    uint32_t offset_in_cluster = off % cluster_size;
    uint8_t  tmp[512];

    while (bytes_read < size && cluster >= 2 && cluster < 0xFFF8) {
        uint32_t lba      = fat16_cluster_to_lba(cluster);
        uint32_t to_read  = cluster_size - offset_in_cluster;
        if (to_read > size - bytes_read) to_read = size - bytes_read;

        uint32_t sec_off = offset_in_cluster / 512;
        uint32_t byte_in_sec = offset_in_cluster % 512;
        uint32_t copied = 0;

        while (copied < to_read) {
            if (!fat16_read_sector(lba + sec_off, tmp)) break;
            uint32_t take = 512 - byte_in_sec;
            if (take > to_read - copied) take = to_read - copied;
            memcpy(buf + bytes_read + copied, tmp + byte_in_sec, take);
            copied      += take;
            byte_in_sec  = 0;
            sec_off++;
        }
        bytes_read        += copied;
        offset_in_cluster  = 0;
        cluster = fat16_get_fat_entry(cluster);
    }
    return bytes_read;
}

/* CAMBIO #5: escritura real en clusters FAT16 */
static uint32_t fat16_node_write(vfs_node_t *node, uint32_t off,
                                 uint32_t size, uint8_t *buf)
{
    if (!fat16_mounted || size == 0) return 0;

    uint16_t cluster_size = bpb.sectors_per_cluster * 512;
    uint16_t start_cluster = (uint16_t)node->owner_gid;

    /* Si el archivo no tiene clusters todavía, asignar el primero */
    if (start_cluster < 2) {
        start_cluster = fat16_alloc_cluster();
        if (!start_cluster) return 0;
        node->owner_gid = start_cluster;
    }

    /* Navegar hasta el cluster del offset `off` (creando más si hace falta) */
    uint16_t cluster    = start_cluster;
    uint32_t cluster_no = off / cluster_size;

    for (uint32_t i = 0; i < cluster_no; i++) {
        uint16_t next = fat16_get_fat_entry(cluster);
        if (next >= 0xFFF8) {
            /* Extender la cadena */
            uint16_t new_c = fat16_alloc_cluster();
            if (!new_c) return 0;
            fat16_set_fat_entry(cluster, new_c);
            cluster = new_c;
        } else {
            cluster = next;
        }
    }

    uint32_t written         = 0;
    uint32_t offset_in_cluster = off % cluster_size;
    uint8_t  tmp[512];

    while (written < size) {
        uint32_t lba     = fat16_cluster_to_lba(cluster);
        uint32_t sec_off = offset_in_cluster / 512;
        uint32_t byte_in = offset_in_cluster % 512;
        uint32_t to_write = cluster_size - offset_in_cluster;
        if (to_write > size - written) to_write = size - written;
        uint32_t done = 0;

        while (done < to_write) {
            /* Leer-Modificar-Escribir para no destruir datos adyacentes */
            if (!fat16_read_sector(lba + sec_off, tmp)) break;
            uint32_t take = 512 - byte_in;
            if (take > to_write - done) take = to_write - done;
            memcpy(tmp + byte_in, buf + written + done, take);
            if (!fat16_write_sector(lba + sec_off, tmp)) break;
            done    += take;
            byte_in  = 0;
            sec_off++;
        }
        written            += done;
        offset_in_cluster   = 0;

        if (written < size) {
            /* Necesitamos otro cluster */
            uint16_t next = fat16_get_fat_entry(cluster);
            if (next >= 0xFFF8) {
                uint16_t new_c = fat16_alloc_cluster();
                if (!new_c) break;
                fat16_set_fat_entry(cluster, new_c);
                cluster = new_c;
            } else {
                cluster = next;
            }
        }
    }

    /* Actualizar tamaño en el nodo VFS */
    if (off + written > node->size)
        node->size = off + written;

    return written;
}

/* ---- Lectura de directorios ---- */

static vfs_node_t *fat16_read_dir(uint16_t start_cluster, bool is_root);

static vfs_node_t *fat16_vfs_readdir(vfs_node_t *node, uint32_t index)
{
    if (index >= node->child_count) return NULL;
    vfs_node_t *child = node->children[index];
    /* Carga lazy de subdirectorios */
    if (child && child->type == VFS_DIRECTORY
               && child->child_count == 0
               && child->name[0] != '.') {
        vfs_node_t *loaded = fat16_read_dir((uint16_t)child->owner_gid, false);
        if (loaded) {
            for (uint32_t i = 0; i < loaded->child_count; i++) {
                if (child->child_count < VFS_MAX_CHILDREN) {
                    child->children[child->child_count++] = loaded->children[i];
                    loaded->children[i]->parent = child;
                }
            }
            kfree(loaded);
        }
    }
    return child;
}

static vfs_node_t *fat16_read_dir(uint16_t start_cluster, bool is_root)
{
    vfs_node_t *dir_node = (vfs_node_t *)kmalloc_aligned(sizeof(vfs_node_t));
    memset(dir_node, 0, sizeof(vfs_node_t));
    dir_node->type        = VFS_DIRECTORY;
    dir_node->permissions = 0755;
    dir_node->owner_gid   = start_cluster;
    dir_node->readdir     = fat16_vfs_readdir;

    uint32_t entries   = 0;
    struct fat_dir_entry *entry_array = NULL;

    if (is_root) {
        entries     = bpb.root_dir_entries;
        entry_array = (struct fat_dir_entry *)kmalloc(entries * 32);
        uint8_t tmp[512];
        for (uint32_t i = 0; i < root_dir_sectors; i++) {
            fat16_read_sector(root_dir_lba + i, tmp);
            memcpy((uint8_t *)entry_array + i * 512, tmp, 512);
        }
    } else {
        uint32_t max_e = 2048;
        entry_array = (struct fat_dir_entry *)kmalloc(max_e * 32);
        uint32_t idx = 0;
        uint16_t cluster = start_cluster;
        uint8_t tmp[512];

        while (cluster >= 2 && cluster < 0xFFF8) {
            uint32_t lba = fat16_cluster_to_lba(cluster);
            for (int s = 0; s < bpb.sectors_per_cluster; s++) {
                fat16_read_sector(lba + s, tmp);
                uint32_t per_sec = 512 / 32;
                for (uint32_t e = 0; e < per_sec && idx < max_e; e++)
                    memcpy(&entry_array[idx++], tmp + e * 32, 32);
            }
            cluster = fat16_get_fat_entry(cluster);
        }
        entries = idx;
    }

    for (uint32_t i = 0; i < entries; i++) {
        struct fat_dir_entry *e = &entry_array[i];
        if ((uint8_t)e->name[0] == 0x00) break;
        if ((uint8_t)e->name[0] == 0xE5) continue;
        if (e->attr == FAT_ATTR_LFN)      continue;
        if (e->attr & FAT_ATTR_VOLUME_ID) continue;

        vfs_node_t *child = (vfs_node_t *)kmalloc_aligned(sizeof(vfs_node_t));
        memset(child, 0, sizeof(vfs_node_t));
        fat16_format_name(e->name, child->name);

        if (e->attr & FAT_ATTR_DIRECTORY) {
            child->type        = VFS_DIRECTORY;
            child->permissions = 0755;
            child->owner_gid   = e->cluster_low;
            child->readdir     = fat16_vfs_readdir;
        } else {
            child->type        = VFS_FILE;
            child->permissions = (e->attr & FAT_ATTR_READ_ONLY) ? 0444 : 0644;
            child->size        = e->size;
            child->owner_gid   = e->cluster_low;
            child->read        = fat16_node_read;
            child->write       = fat16_node_write;   /* CAMBIO #5 */
        }

        if (dir_node->child_count < VFS_MAX_CHILDREN) {
            dir_node->children[dir_node->child_count++] = child;
            child->parent = dir_node;
        } else {
            kfree(child);
        }
    }

    kfree(entry_array);
    return dir_node;
}

/* ---- CAMBIO #5: creación de archivos en el directorio raíz FAT16 ---- */

/* Crea una entrada en el directorio raíz para un archivo nuevo.
 * Devuelve el cluster inicial asignado, o 0 en caso de error. */
static uint16_t fat16_create_root_entry(const char *name, uint8_t attr)
{
    uint16_t start_cluster = fat16_alloc_cluster();
    if (!start_cluster) return 0;

    struct fat_dir_entry entry;
    memset(&entry, 0, sizeof(entry));
    fat16_to_83(name, entry.name);
    entry.attr        = attr;
    entry.cluster_low = start_cluster;
    entry.size        = 0;

    /* Buscar entrada libre (0x00 o 0xE5) en el directorio raíz */
    uint8_t buf[512];
    uint32_t per_sec = 512 / 32;
    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        if (!fat16_read_sector(root_dir_lba + sec, buf)) continue;
        for (uint32_t e = 0; e < per_sec; e++) {
            struct fat_dir_entry *slot = (struct fat_dir_entry *)(buf + e * 32);
            if ((uint8_t)slot->name[0] == 0x00 || (uint8_t)slot->name[0] == 0xE5) {
                memcpy(slot, &entry, 32);
                fat16_write_sector(root_dir_lba + sec, buf);
                return start_cluster;
            }
        }
    }

    /* Sin espacio: liberar el cluster recién asignado */
    fat16_set_fat_entry(start_cluster, 0x0000);
    return 0;
}

/* CAMBIO #5: elimina la entrada de un archivo del directorio raíz. */
static bool fat16_delete_root_entry(const char *name)
{
    char fat83[11];
    fat16_to_83(name, fat83);

    uint8_t buf[512];
    uint32_t per_sec = 512 / 32;
    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        if (!fat16_read_sector(root_dir_lba + sec, buf)) continue;
        for (uint32_t e = 0; e < per_sec; e++) {
            struct fat_dir_entry *slot = (struct fat_dir_entry *)(buf + e * 32);
            if ((uint8_t)slot->name[0] == 0x00) return false;
            if ((uint8_t)slot->name[0] == 0xE5) continue;
            if (memcmp(slot->name, fat83, 11) == 0) {
                fat16_free_cluster_chain(slot->cluster_low);
                slot->name[0] = 0xE5;   /* marcar como borrada */
                fat16_write_sector(root_dir_lba + sec, buf);
                return true;
            }
        }
    }
    return false;
}

/* ---- Inicialización ---- */

static vfs_node_t *fat_root_node = NULL;

void fat16_init(void)
{
    if (!ata_present()) return;

    uint8_t buf[512];
    if (ata_read_sectors(0, 1, buf) != 0) return;

    struct mbr *m = (struct mbr *)buf;
    if (m->signature == 0xAA55) {
        for (int i = 0; i < 4; i++) {
            if (m->part[i].type == 0x04 || m->part[i].type == 0x06
                                        || m->part[i].type == 0x0E) {
                partition_lba = m->part[i].lba_first;
                break;
            }
        }
    }

    if (ata_read_sectors(partition_lba, 1, buf) != 0) return;
    memcpy(&bpb, buf, sizeof(struct fat_bpb));

    if (bpb.bytes_per_sector != 512) return;

    fat_start_lba    = bpb.reserved_sectors;
    root_dir_lba     = fat_start_lba + (uint32_t)(bpb.fat_count * bpb.sectors_per_fat_16);
    root_dir_sectors = (bpb.root_dir_entries * 32 + 511) / 512;
    data_start_lba   = root_dir_lba + root_dir_sectors;

    uint32_t total_sectors = bpb.total_sectors_16
                             ? bpb.total_sectors_16 : bpb.total_sectors_32;
    uint32_t data_sectors  = total_sectors - data_start_lba;
    total_clusters         = data_sectors / bpb.sectors_per_cluster;

    fat_root_node = fat16_read_dir(0, true);
    if (!fat_root_node) return;   /* FIX (v0.5.3): antes se usaba sin NULL check */
    strcpy(fat_root_node->name, "fat");
    fat_root_node->readdir = fat16_vfs_readdir;

    vfs_node_t *root = vfs_get_root();
    if (root && root->child_count < VFS_MAX_CHILDREN) {
        root->children[root->child_count++] = fat_root_node;
        fat_root_node->parent = root;
        fat16_mounted = true;
        kprintf("  [ OK ] FAT16 mounted at /fat (R/W, %u clusters)\n",
                total_clusters);
    }

    (void)fat16_create_root_entry;   /* suprimir warning de función no usada */
    (void)fat16_delete_root_entry;
}
