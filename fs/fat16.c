#include "fat16.h"
#include "../drivers/ata.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"

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

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

static uint32_t partition_lba = 0;
static struct fat_bpb bpb;
static uint32_t fat_start_lba;
static uint32_t root_dir_lba;
static uint32_t data_start_lba;
static uint32_t root_dir_sectors;

static bool fat16_read_sector(uint32_t lba, uint8_t* buf) {
    return ata_read_sectors(partition_lba + lba, 1, buf) == 0;
}

static uint16_t fat16_get_fat_entry(uint16_t cluster) {
    uint32_t fat_offset = cluster * 2;
    uint32_t sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset = fat_offset % 512;
    
    uint8_t buf[512];
    if (!fat16_read_sector(sector, buf)) return 0xFFFF;
    
    return *((uint16_t*)&buf[offset]);
}

static uint32_t fat16_cluster_to_lba(uint16_t cluster) {
    return data_start_lba + ((cluster - 2) * bpb.sectors_per_cluster);
}

static void fat16_format_name(const char* fat_name, char* out) {
    int out_idx = 0;
    int i;
    for (i = 0; i < 8 && fat_name[i] != ' '; i++) {
        out[out_idx++] = (fat_name[i] >= 'A' && fat_name[i] <= 'Z') ? fat_name[i] + 32 : fat_name[i];
    }
    if (fat_name[8] != ' ') {
        out[out_idx++] = '.';
        for (i = 8; i < 11 && fat_name[i] != ' '; i++) {
            out[out_idx++] = (fat_name[i] >= 'A' && fat_name[i] <= 'Z') ? fat_name[i] + 32 : fat_name[i];
        }
    }
    out[out_idx] = 0;
}

static uint32_t fat16_node_read(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf) {
    if (off >= node->size) return 0;
    if (off + size > node->size) size = node->size - off;
    
    uint16_t cluster = (uint16_t)node->owner_gid; // We'll store start cluster in owner_gid as a hack
    if (cluster < 2) return 0;
    
    uint32_t cluster_size = bpb.sectors_per_cluster * 512;
    uint32_t cluster_skip = off / cluster_size;
    
    for (uint32_t i = 0; i < cluster_skip; i++) {
        cluster = fat16_get_fat_entry(cluster);
        if (cluster >= 0xFFF8) return 0;
    }
    
    uint32_t bytes_read = 0;
    uint32_t offset_in_cluster = off % cluster_size;
    
    while (bytes_read < size && cluster >= 2 && cluster < 0xFFF8) {
        uint32_t lba = fat16_cluster_to_lba(cluster);
        uint8_t cluster_buf[512 * 64]; // Max 32KB cluster
        
        for (int s = 0; s < bpb.sectors_per_cluster; s++) {
            fat16_read_sector(lba + s, cluster_buf + s * 512);
        }
        
        uint32_t to_read = cluster_size - offset_in_cluster;
        if (to_read > size - bytes_read) to_read = size - bytes_read;
        
        memcpy(buf + bytes_read, cluster_buf + offset_in_cluster, to_read);
        bytes_read += to_read;
        
        offset_in_cluster = 0;
        cluster = fat16_get_fat_entry(cluster);
    }
    
    return bytes_read;
}

static vfs_node_t* fat16_read_dir(uint16_t start_cluster, bool is_root) {
    vfs_node_t* dir_node = (vfs_node_t*)kmalloc_aligned(sizeof(vfs_node_t));
    memset(dir_node, 0, sizeof(vfs_node_t));
    dir_node->type = VFS_DIRECTORY;
    dir_node->permissions = 0755;
    dir_node->owner_gid = start_cluster;
    
    uint8_t buf[512 * 64];
    uint32_t entries = 0;
    struct fat_dir_entry* entry_array = NULL;
    
    if (is_root) {
        entries = bpb.root_dir_entries;
        entry_array = (struct fat_dir_entry*)kmalloc(entries * sizeof(struct fat_dir_entry));
        for (uint32_t i = 0; i < root_dir_sectors; i++) {
            fat16_read_sector(root_dir_lba + i, buf);
            memcpy((uint8_t*)entry_array + i * 512, buf, 512);
        }
    } else {
        // Read directory cluster chain
        uint16_t cluster = start_cluster;
        uint32_t max_entries = 4096; // arbitrary limit
        entry_array = (struct fat_dir_entry*)kmalloc(max_entries * sizeof(struct fat_dir_entry));
        uint32_t idx = 0;
        
        while (cluster >= 2 && cluster < 0xFFF8) {
            uint32_t lba = fat16_cluster_to_lba(cluster);
            for (int s = 0; s < bpb.sectors_per_cluster; s++) {
                fat16_read_sector(lba + s, buf);
                for (int e = 0; e < 512 / sizeof(struct fat_dir_entry); e++) {
                    if (idx < max_entries) {
                        memcpy(&entry_array[idx++], buf + e * sizeof(struct fat_dir_entry), sizeof(struct fat_dir_entry));
                    }
                }
            }
            cluster = fat16_get_fat_entry(cluster);
        }
        entries = idx;
    }
    
    for (uint32_t i = 0; i < entries; i++) {
        struct fat_dir_entry* e = &entry_array[i];
        if (e->name[0] == 0x00) break; // End of directory
        if ((uint8_t)e->name[0] == 0xE5) continue; // Deleted
        if (e->attr == FAT_ATTR_LFN) continue; // LFN not supported
        if (e->attr & FAT_ATTR_VOLUME_ID) continue; // Volume label
        
        vfs_node_t* child = (vfs_node_t*)kmalloc_aligned(sizeof(vfs_node_t));
        memset(child, 0, sizeof(vfs_node_t));
        
        fat16_format_name(e->name, child->name);
        
        if (e->attr & FAT_ATTR_DIRECTORY) {
            child->type = VFS_DIRECTORY;
            child->permissions = 0755;
            child->owner_gid = e->cluster_low; // Store cluster
        } else {
            child->type = VFS_FILE;
            child->permissions = (e->attr & FAT_ATTR_READ_ONLY) ? 0444 : 0666;
            child->size = e->size;
            child->owner_gid = e->cluster_low;
            child->read = fat16_node_read;
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

static vfs_node_t* fat_root_node = NULL;

static vfs_node_t* fat16_vfs_readdir(vfs_node_t* node, uint32_t index) {
    if (index >= node->child_count) return NULL;
    // We lazy-load directories to save RAM, but for this demo we preloaded them
    // Actually, we didn't recurse. Let's do it lazy.
    vfs_node_t* child = node->children[index];
    if (child && child->type == VFS_DIRECTORY && child->child_count == 0 && child->name[0] != '.') {
        // Populate
        vfs_node_t* loaded = fat16_read_dir((uint16_t)child->owner_gid, false);
        for(uint32_t i=0; i<loaded->child_count; i++) {
            if (child->child_count < VFS_MAX_CHILDREN) {
                child->children[child->child_count++] = loaded->children[i];
                loaded->children[i]->parent = child;
            }
        }
        kfree(loaded); // free dummy wrapper
    }
    return child;
}

void fat16_init(void) {
    if (!ata_present()) return;
    
    uint8_t buf[512];
    if (ata_read_sectors(0, 1, buf) != 0) return;
    
    struct mbr* m = (struct mbr*)buf;
    if (m->signature == 0xAA55) {
        // Look for FAT16 partition
        for (int i = 0; i < 4; i++) {
            if (m->part[i].type == 0x04 || m->part[i].type == 0x06 || m->part[i].type == 0x0E) {
                partition_lba = m->part[i].lba_first;
                break;
            }
        }
    }
    
    if (ata_read_sectors(partition_lba, 1, buf) != 0) return;
    
    memcpy(&bpb, buf, sizeof(struct fat_bpb));
    
    if (strncmp(bpb.fs_type, "FAT16", 5) != 0) {
        // Check if FAT12 or just fallback heuristics
        if (bpb.bytes_per_sector != 512) return;
    }
    
    fat_start_lba = bpb.reserved_sectors;
    root_dir_lba = fat_start_lba + (bpb.fat_count * bpb.sectors_per_fat_16);
    root_dir_sectors = (bpb.root_dir_entries * sizeof(struct fat_dir_entry) + 511) / 512;
    data_start_lba = root_dir_lba + root_dir_sectors;
    
    fat_root_node = fat16_read_dir(0, true);
    strcpy(fat_root_node->name, "fat");
    fat_root_node->readdir = fat16_vfs_readdir;
    
    // Mount it at /fat
    vfs_node_t* root = vfs_get_root();
    if (root && root->child_count < VFS_MAX_CHILDREN) {
        root->children[root->child_count++] = fat_root_node;
        fat_root_node->parent = root;
        kprintf("  [ OK ] FAT16 partition mounted at /fat\n");
    }
}
