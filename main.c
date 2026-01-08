#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <string.h>

#define F_CPU           16000000UL  
#define BAUD_RATE       9600        
#define UBRR_VALUE      ((F_CPU / (16UL * BAUD_RATE)) - 1)  

#define EEPROM_SIZE     1024        
#define PAGE_SIZE       64          
#define TOTAL_PAGES     (EEPROM_SIZE / PAGE_SIZE)  
#define CACHE_SIZE      2           

#define WAL_PAGE_ID         15      
#define WAL_PAGE_ADDR       (WAL_PAGE_ID * PAGE_SIZE)  
#define DATA_PAGES          15      
#define MAX_LOG_ENTRIES     8       

#define WAL_STATUS_EMPTY     0xFF   
#define WAL_STATUS_PENDING   0x01   
#define WAL_STATUS_COMMITTED 0x02   

#define BTREE_DEGREE        3                           
#define BTREE_MAX_KEYS      (2 * BTREE_DEGREE - 1)      
#define BTREE_MIN_KEYS      (BTREE_DEGREE - 1)          
#define BTREE_MAX_CHILDREN  (2 * BTREE_DEGREE)          

#define BTREE_ROOT_PAGE     0                           
#define BTREE_INVALID_PAGE  0xFF                        
#define BTREE_INVALID_KEY   0xFF                        

typedef struct {
    uint8_t  is_leaf;                   
    uint8_t  num_keys;                  
    uint8_t  padding;                   
    uint8_t  keys[BTREE_MAX_KEYS];      
    uint8_t  children[BTREE_MAX_CHILDREN]; 
    uint16_t values[BTREE_MAX_KEYS];    
} BTreeNode;

_Static_assert(sizeof(BTreeNode) <= PAGE_SIZE, "BTreeNode exceeds PAGE_SIZE");

static uint8_t next_free_page = 1;  

typedef struct {
    uint8_t transaction_id;   
    uint8_t target_page_id;   
    uint8_t offset;           
    uint8_t old_value;        
    uint8_t new_value;        
    uint8_t status;           
    uint8_t checksum;         
    uint8_t reserved;         
} LogEntry;

_Static_assert(sizeof(LogEntry) == 8, "LogEntry must be 8 bytes");
_Static_assert(sizeof(LogEntry) * MAX_LOG_ENTRIES <= PAGE_SIZE, 
               "LogEntry array must fit in WAL page");

static uint8_t wal_active = 0;         
static uint8_t wal_transaction_id = 0;  
static uint8_t wal_log_slot = 0;        

typedef struct {
    uint8_t page_id;            
    uint8_t dirty;              
    uint8_t valid;              
    uint8_t lru_counter;        
    uint8_t data[PAGE_SIZE];    
} CachedPage;

typedef struct {
    CachedPage slots[CACHE_SIZE];   
    uint8_t global_counter;          
} PageCache;

static PageCache cache;

#define CMD_BUFFER_SIZE 32
static char cmd_buffer[CMD_BUFFER_SIZE];
static uint8_t cmd_index = 0;

static uint8_t eeprom_read_byte(uint16_t address) {
    
    while (EECR & (1 << EEPE)) {
        
    }

    EEAR = address;

    EECR |= (1 << EERE);

    return EEDR;
}

static void eeprom_write_byte(uint16_t address, uint8_t data) {
    
    while (EECR & (1 << EEPE)) {
        
    }

    EEAR = address;
    EEDR = data;

    uint8_t sreg_save = SREG;   
    cli();                       
    
    EECR |= (1 << EEMPE);       
    EECR |= (1 << EEPE);        
    
    SREG = sreg_save;           

}

static void eeprom_read_block(uint16_t address, uint8_t *buffer, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        buffer[i] = eeprom_read_byte(address + i);
    }
}

static void eeprom_write_block(uint16_t address, const uint8_t *buffer, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        eeprom_write_byte(address + i, buffer[i]);
    }
}

static void uart_print(const char *str);
static void uart_println(const char *str);
static void uart_print_hex8(uint8_t val);
static void uart_print_dec16(uint16_t val);

static uint8_t wal_calculate_checksum(LogEntry *entry) {
    uint8_t checksum = 0;
    checksum ^= entry->transaction_id;
    checksum ^= entry->target_page_id;
    checksum ^= entry->offset;
    checksum ^= entry->old_value;
    checksum ^= entry->new_value;
    checksum ^= entry->status;
    
    return checksum;
}

static uint8_t wal_verify_checksum(LogEntry *entry) {
    return (wal_calculate_checksum(entry) == entry->checksum);
}

static void wal_read_entry(uint8_t slot, LogEntry *entry) {
    uint16_t addr = WAL_PAGE_ADDR + (slot * sizeof(LogEntry));
    eeprom_read_block(addr, (uint8_t *)entry, sizeof(LogEntry));
}

static void wal_write_entry(uint8_t slot, LogEntry *entry) {
    uint16_t addr = WAL_PAGE_ADDR + (slot * sizeof(LogEntry));
    eeprom_write_block(addr, (uint8_t *)entry, sizeof(LogEntry));
}

static void wal_clear_log(void) {
    for (uint8_t i = 0; i < PAGE_SIZE; i++) {
        eeprom_write_byte(WAL_PAGE_ADDR + i, 0xFF);
    }
    wal_log_slot = 0;
}

static void wal_begin(void) {
    if (wal_active) {
        uart_println("[W]Active");
        return;
    }
    
    wal_active = 1;
    wal_transaction_id++;  
    wal_log_slot = 0;      
    
    uart_print("[W]B ");
    uart_print_dec16(wal_transaction_id);
    }

static uint8_t wal_log_change(uint8_t page_id, uint8_t offset, 
                               uint8_t old_value, uint8_t new_value) {
    if (!wal_active) {
        uart_println("[W]NoTxn");
        return 0;
    }
    
    if (wal_log_slot >= MAX_LOG_ENTRIES) {
        uart_println("[W]Full");
        return 0;
    }

    LogEntry entry;
    entry.transaction_id = wal_transaction_id;
    entry.target_page_id = page_id;
    entry.offset = offset;
    entry.old_value = old_value;
    entry.new_value = new_value;
    entry.status = WAL_STATUS_PENDING;  
    entry.reserved = 0;
    entry.checksum = wal_calculate_checksum(&entry);

    wal_write_entry(wal_log_slot, &entry);
    
        uart_print_dec16(page_id);
        uart_print_dec16(offset);
                        
    wal_log_slot++;
    return 1;
}

static void wal_commit(void) {
    if (!wal_active) {
        uart_println("[WAL] ERROR: No active transaction to commit!");
        return;
    }
    
    uart_print("[W]C ");
    uart_print_dec16(wal_transaction_id);
    uart_println("...");

    for (uint8_t i = 0; i < wal_log_slot; i++) {
        LogEntry entry;
        wal_read_entry(i, &entry);
        
        if (entry.transaction_id == wal_transaction_id &&
            entry.status == WAL_STATUS_PENDING) {
            entry.status = WAL_STATUS_COMMITTED;
            entry.checksum = wal_calculate_checksum(&entry);
            wal_write_entry(i, &entry);
        }
    }

    wal_active = 0;
    
    uart_println("[W]OK");

    wal_clear_log();
}

static void wal_rollback(void) {
    if (!wal_active) {
        return;  
    }
    
    uart_print("[W]R ");
    uart_print_dec16(wal_transaction_id);
    uart_println("...");

    for (int8_t i = wal_log_slot - 1; i >= 0; i--) {
        LogEntry entry;
        wal_read_entry(i, &entry);
        
        if (entry.transaction_id == wal_transaction_id &&
            entry.status == WAL_STATUS_PENDING) {
            
            uint16_t addr = (uint16_t)entry.target_page_id * PAGE_SIZE + entry.offset;
            eeprom_write_byte(addr, entry.old_value);
            
                        uart_print_dec16(entry.target_page_id);
                        uart_print_dec16(entry.offset);
                                            }
    }
    
    wal_active = 0;
    wal_clear_log();
    
    uart_println("[W]RB");
}

static void db_recovery(void) {
                    
    uint8_t pending_found = 0;
    uint8_t corrupted_found = 0;
    uint8_t committed_found = 0;

        
    for (uint8_t i = 0; i < MAX_LOG_ENTRIES; i++) {
        LogEntry entry;
        wal_read_entry(i, &entry);

        if (entry.status == WAL_STATUS_EMPTY) {
            continue;
        }
        
                uart_print_dec16(i);
        uart_print(": txn=");
        uart_print_dec16(entry.transaction_id);
        uart_print("p");
        uart_print_dec16(entry.target_page_id);
        uart_print(" status=");

        if (!wal_verify_checksum(&entry)) {
            uart_println("BAD");
            corrupted_found++;
            continue;
        }

        if (entry.status == WAL_STATUS_PENDING) {
            uart_println("UNDO");
            pending_found++;

            uint16_t addr = (uint16_t)entry.target_page_id * PAGE_SIZE + entry.offset;

            uint8_t current = eeprom_read_byte(addr);
            
                                                uart_print(" current=0x");
                        uart_print(" -> restoring=0x");
                        
            eeprom_write_byte(addr, entry.old_value);
            
        } else if (entry.status == WAL_STATUS_COMMITTED) {
            uart_println("OK");
            committed_found++;
            
        } else {
            uart_print("UNKNOWN (0x");
                        uart_println(") - skipping");
        }
    }

                uart_print_dec16(pending_found);
            uart_print_dec16(committed_found);
            uart_print_dec16(corrupted_found);
        
    if (pending_found > 0) {
                    } else {
            }

        wal_clear_log();

    wal_active = 0;
    wal_transaction_id = 0;
    wal_log_slot = 0;
    
            }

static uint8_t wal_is_active(void) {
    return wal_active;
}

static void uart_init(void) {
    
    UBRR0H = (uint8_t)(UBRR_VALUE >> 8);
    UBRR0L = (uint8_t)(UBRR_VALUE);

    UCSR0B = (1 << TXEN0) | (1 << RXEN0);

    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static void uart_tx_byte(uint8_t data) {
    
    while (!(UCSR0A & (1 << UDRE0))) {
        
    }
    UDR0 = data;  
}

static uint8_t uart_rx_byte(void) {
    
    while (!(UCSR0A & (1 << RXC0))) {
        
    }
    return UDR0;  
}

static uint8_t uart_rx_available(void) {
    return (UCSR0A & (1 << RXC0)) ? 1 : 0;
}

static void uart_print(const char *str) {
    while (*str) {
        uart_tx_byte(*str++);
    }
}

static void uart_println(const char *str) {
    uart_print(str);
    uart_tx_byte('\r');
    uart_tx_byte('\n');
}

static void uart_print_hex8(uint8_t val) {
    const char hex[] = "0123456789ABCDEF";
    uart_tx_byte(hex[(val >> 4) & 0x0F]);
    uart_tx_byte(hex[val & 0x0F]);
}

static void uart_print_dec16(uint16_t val) {
    char buf[6];  
    int8_t i = 5;
    
    buf[5] = '\0';
    
    if (val == 0) {
        uart_tx_byte('0');
        return;
    }
    
    while (val > 0 && i > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    
    uart_print(&buf[i]);
}

static void cache_init(void) {
    for (uint8_t i = 0; i < CACHE_SIZE; i++) {
        cache.slots[i].valid = 0;
        cache.slots[i].dirty = 0;
        cache.slots[i].page_id = 0xFF;  
        cache.slots[i].lru_counter = 0;
    }
    cache.global_counter = 0;
}

static uint8_t cache_find_slot(uint8_t page_id) {
    for (uint8_t i = 0; i < CACHE_SIZE; i++) {
        if (cache.slots[i].valid && cache.slots[i].page_id == page_id) {
            return i;
        }
    }
    return 0xFF;  
}

static uint8_t cache_find_victim(void) {
    uint8_t victim = 0;
    uint8_t oldest_counter = 0xFF;

    for (uint8_t i = 0; i < CACHE_SIZE; i++) {
        if (!cache.slots[i].valid) {
            
            return i;
        }
        if (cache.slots[i].lru_counter < oldest_counter) {
            oldest_counter = cache.slots[i].lru_counter;
            victim = i;
        }
    }
    
    return victim;
}

static void flush_page(uint8_t slot) {
    if (!cache.slots[slot].valid || !cache.slots[slot].dirty) {
        return;  
    }
    
    uint16_t eeprom_addr = (uint16_t)cache.slots[slot].page_id * PAGE_SIZE;
    
        uart_print_dec16(cache.slots[slot].page_id);
    uart_print(" to EEPROM addr 0x");
                
    eeprom_write_block(eeprom_addr, cache.slots[slot].data, PAGE_SIZE);
    
    cache.slots[slot].dirty = 0;  
}

static void flush_cache(void) {
        
    for (uint8_t i = 0; i < CACHE_SIZE; i++) {
        flush_page(i);
    }
    
    }

static uint8_t* read_page(uint8_t page_id) {
    
    uint8_t slot = cache_find_slot(page_id);
    
    if (slot != 0xFF) {
        
        cache.slots[slot].lru_counter = ++cache.global_counter;
                uart_print_dec16(page_id);
                return cache.slots[slot].data;
    }

        uart_print_dec16(page_id);
    
    slot = cache_find_victim();

    if (cache.slots[slot].valid && cache.slots[slot].dirty) {
                uart_print_dec16(cache.slots[slot].page_id);
                flush_page(slot);
    }

    uint16_t eeprom_addr = (uint16_t)page_id * PAGE_SIZE;
    eeprom_read_block(eeprom_addr, cache.slots[slot].data, PAGE_SIZE);

    cache.slots[slot].page_id = page_id;
    cache.slots[slot].valid = 1;
    cache.slots[slot].dirty = 0;  
    cache.slots[slot].lru_counter = ++cache.global_counter;
    
    return cache.slots[slot].data;
}

static void write_page(uint8_t page_id, const uint8_t *data) {
    
    if (page_id == WAL_PAGE_ID) {
        uart_println("[PAGER] ERROR: Cannot write to WAL page via write_page!");
        return;
    }
    
    if (page_id >= DATA_PAGES) {
        uart_println("[PAGER] ERROR: Page ID out of range!");
        return;
    }

    uint8_t slot = cache_find_slot(page_id);
    
    if (slot == 0xFF) {

        (void)read_page(page_id);  
        slot = cache_find_slot(page_id);
    }

    memcpy(cache.slots[slot].data, data, PAGE_SIZE);

    cache.slots[slot].dirty = 1;
    cache.slots[slot].lru_counter = ++cache.global_counter;
    
        uart_print_dec16(page_id);
    uart_println(" D");
}

static void mark_page_dirty(uint8_t page_id) {
    uint8_t slot = cache_find_slot(page_id);
    if (slot != 0xFF) {
        cache.slots[slot].dirty = 1;
    }
}

static uint8_t btree_allocate_page(void) {
    
    if (next_free_page >= DATA_PAGES) {
        uart_println("[B]FULL");
        return BTREE_INVALID_PAGE;
    }
        uart_print_dec16(next_free_page);
        return next_free_page++;
}

static void btree_init(void) {
    uart_println("Init");

    uint8_t *page_data = read_page(BTREE_ROOT_PAGE);
    BTreeNode *root = (BTreeNode *)page_data;

    root->is_leaf = 1;
    root->num_keys = 0;
    root->padding = 0;

    for (uint8_t i = 0; i < BTREE_MAX_KEYS; i++) {
        root->keys[i] = BTREE_INVALID_KEY;
        root->values[i] = 0;
    }
    for (uint8_t i = 0; i < BTREE_MAX_CHILDREN; i++) {
        root->children[i] = BTREE_INVALID_PAGE;
    }
    
    mark_page_dirty(BTREE_ROOT_PAGE);
    next_free_page = 1;  
    
    uart_println("Root");
}

static uint8_t btree_search_in_node(BTreeNode *node, uint8_t key, uint8_t *found) {
    uint8_t i = 0;

    while (i < node->num_keys && key > node->keys[i]) {
        i++;
    }

    if (i < node->num_keys && key == node->keys[i]) {
        *found = 1;
    } else {
        *found = 0;
    }
    
    return i;
}

static uint8_t btree_search(uint8_t key, uint16_t *out_value) {
    uart_print("[B]? ");
    uart_print_dec16(key);
        
    uint8_t current_page = BTREE_ROOT_PAGE;

    while (1) {
        uint8_t *page_data = read_page(current_page);
        BTreeNode *node = (BTreeNode *)page_data;
        
                uart_print_dec16(current_page);
        uart_print(node->is_leaf ? " (leaf)" : " (internal)");
        uart_print(" keys=");
        uart_print_dec16(node->num_keys);
        
        uint8_t found = 0;
        uint8_t idx = btree_search_in_node(node, key, &found);
        
        if (found) {
            if (node->is_leaf) {
                
                *out_value = node->values[idx];
                uart_print("[B]OK k=");
                uart_print_dec16(key);
                uart_print("=");
                uart_print_dec16(*out_value);
                                return 1;
            } else {
                
                current_page = node->children[idx + 1];
            }
        } else {
            if (node->is_leaf) {
                
                uart_println("[B]NF");
                return 0;
            } else {
                
                if (node->children[idx] == BTREE_INVALID_PAGE) {
                    uart_println("[BTREE] Invalid child pointer!");
                    return 0;
                }
                current_page = node->children[idx];
            }
        }
    }
}

static void btree_split_child(uint8_t parent_page, uint8_t child_index, uint8_t child_page) {
        uart_print_dec16(child_index);
    uart_print(" (page ");
    uart_print_dec16(child_page);
    uart_println(")");

    uint8_t new_page = btree_allocate_page();
    if (new_page == BTREE_INVALID_PAGE) {
        uart_println("[BTREE] ERROR: Cannot allocate page for split!");
        return;
    }

    uint8_t *parent_data = read_page(parent_page);
    BTreeNode *parent = (BTreeNode *)parent_data;
    
    uint8_t *child_data = read_page(child_page);
    BTreeNode *child = (BTreeNode *)child_data;
    
    uint8_t *new_data = read_page(new_page);
    BTreeNode *new_node = (BTreeNode *)new_data;

    new_node->is_leaf = child->is_leaf;
    new_node->num_keys = BTREE_DEGREE - 1;  
    new_node->padding = 0;

    for (uint8_t i = 0; i < BTREE_MAX_KEYS; i++) {
        new_node->keys[i] = BTREE_INVALID_KEY;
        new_node->values[i] = 0;
    }
    for (uint8_t i = 0; i < BTREE_MAX_CHILDREN; i++) {
        new_node->children[i] = BTREE_INVALID_PAGE;
    }

    for (uint8_t i = 0; i < BTREE_DEGREE - 1; i++) {
        new_node->keys[i] = child->keys[i + BTREE_DEGREE];
        if (child->is_leaf) {
            new_node->values[i] = child->values[i + BTREE_DEGREE];
        }
    }

    if (!child->is_leaf) {
        for (uint8_t i = 0; i < BTREE_DEGREE; i++) {
            new_node->children[i] = child->children[i + BTREE_DEGREE];
        }
    }

    child->num_keys = BTREE_DEGREE - 1;  

    for (int8_t i = parent->num_keys - 1; i >= (int8_t)child_index; i--) {
        parent->keys[i + 1] = parent->keys[i];
    }

    for (int8_t i = parent->num_keys; i >= (int8_t)child_index + 1; i--) {
        parent->children[i + 1] = parent->children[i];
    }

    parent->keys[child_index] = child->keys[BTREE_DEGREE - 1];
    parent->children[child_index + 1] = new_page;
    parent->num_keys++;

    mark_page_dirty(parent_page);
    mark_page_dirty(child_page);
    mark_page_dirty(new_page);
    
        uart_print_dec16(child->keys[BTREE_DEGREE - 1]);
    }

static void btree_insert_nonfull(uint8_t page_id, uint8_t key, uint16_t value) {
    uint8_t *page_data = read_page(page_id);
    BTreeNode *node = (BTreeNode *)page_data;
    
    int8_t i = node->num_keys - 1;  
    
    if (node->is_leaf) {

        while (i >= 0 && key < node->keys[i]) {
            node->keys[i + 1] = node->keys[i];
            node->values[i + 1] = node->values[i];
            i--;
        }

        node->keys[i + 1] = key;
        node->values[i + 1] = value;
        node->num_keys++;
        
        mark_page_dirty(page_id);
        
        uart_print("[B]+ ");
        uart_print_dec16(key);
                uart_print_dec16(page_id);
            } else {

        while (i >= 0 && key < node->keys[i]) {
            i--;
        }
        i++;  
        
        uint8_t child_page = node->children[i];

        uint8_t *child_data = read_page(child_page);
        BTreeNode *child = (BTreeNode *)child_data;
        
        if (child->num_keys == BTREE_MAX_KEYS) {
            
                        btree_split_child(page_id, i, child_page);

            page_data = read_page(page_id);
            node = (BTreeNode *)page_data;
            
            if (key > node->keys[i]) {
                i++;  
            }
        }

        btree_insert_nonfull(node->children[i], key, value);
    }
}

static uint8_t btree_insert(uint8_t key, uint16_t value) {
    uart_print("[B]I k=");
    uart_print_dec16(key);
    uart_print("=");
    uart_print_dec16(value);
    
    uint8_t *root_data = read_page(BTREE_ROOT_PAGE);
    BTreeNode *root = (BTreeNode *)root_data;
    
    if (root->num_keys == BTREE_MAX_KEYS) {
        
        
        uint8_t old_root_page = btree_allocate_page();
        if (old_root_page == BTREE_INVALID_PAGE) {
            uart_println("[BTREE] ERROR: Cannot allocate page for root split!");
            return 0;
        }

        uint8_t *old_root_data = read_page(old_root_page);
        memcpy(old_root_data, root_data, PAGE_SIZE);
        mark_page_dirty(old_root_page);

        root_data = read_page(BTREE_ROOT_PAGE);  
        root = (BTreeNode *)root_data;
        
        root->is_leaf = 0;           
        root->num_keys = 0;          
        root->children[0] = old_root_page;  

        for (uint8_t i = 1; i < BTREE_MAX_CHILDREN; i++) {
            root->children[i] = BTREE_INVALID_PAGE;
        }
        for (uint8_t i = 0; i < BTREE_MAX_KEYS; i++) {
            root->keys[i] = BTREE_INVALID_KEY;
        }
        
        mark_page_dirty(BTREE_ROOT_PAGE);

        btree_split_child(BTREE_ROOT_PAGE, 0, old_root_page);
    }

    btree_insert_nonfull(BTREE_ROOT_PAGE, key, value);
    
    return 1;
}

static uint8_t btree_delete(uint8_t key) {
    uart_print("[B]- ");
    uart_print_dec16(key);
    
    uint8_t current_page = BTREE_ROOT_PAGE;
    
    while (1) {
        uint8_t *page_data = read_page(current_page);
        BTreeNode *node = (BTreeNode *)page_data;
        
        uint8_t found = 0;
        uint8_t idx = btree_search_in_node(node, key, &found);
        
        if (found && node->is_leaf) {

            for (uint8_t i = idx; i < node->num_keys - 1; i++) {
                node->keys[i] = node->keys[i + 1];
                node->values[i] = node->values[i + 1];
            }
            node->keys[node->num_keys - 1] = BTREE_INVALID_KEY;
            node->values[node->num_keys - 1] = 0;
            node->num_keys--;
            
            mark_page_dirty(current_page);
            uart_println("[B]Del");
            return 1;
        } else if (!found && node->is_leaf) {
            uart_println("[B]NF");
            return 0;
        } else {
            
            if (found) {
                current_page = node->children[idx + 1];
            } else {
                current_page = node->children[idx];
            }
            
            if (current_page == BTREE_INVALID_PAGE) {
                uart_println("[BTREE] Invalid tree structure!");
                return 0;
            }
        }
    }
}

static void btree_print_node(uint8_t page_id, uint8_t depth) {
    if (page_id == BTREE_INVALID_PAGE || depth > 5) return;
    
    uint8_t *page_data = read_page(page_id);
    BTreeNode *node = (BTreeNode *)page_data;

    for (uint8_t d = 0; d < depth; d++) {
        uart_print("  ");
    }

    uart_print("Page ");
    uart_print_dec16(page_id);
    uart_print(node->is_leaf ? " [LEAF] " : " [INT]  ");
    uart_print("keys(");
    uart_print_dec16(node->num_keys);
    uart_print("): ");
    
    for (uint8_t i = 0; i < node->num_keys; i++) {
        if (i > 0) uart_print(",");
        uart_print_dec16(node->keys[i]);
        if (node->is_leaf) {
            uart_print("=>");
            uart_print_dec16(node->values[i]);
        }
    }
    
    if (!node->is_leaf) {
        for (uint8_t i = 0; i <= node->num_keys; i++) {
            if (node->children[i] != BTREE_INVALID_PAGE) {
                btree_print_node(node->children[i], depth + 1);
            }
        }
    }
}

static void btree_print(void) {
    uart_println("=TREE=");
    btree_print_node(BTREE_ROOT_PAGE, 0);
    uart_println("===");
}

static void btree_list_inorder(uint8_t page_id, uint8_t *count) {
    if (page_id == BTREE_INVALID_PAGE) return;
    
    uint8_t *page_data = read_page(page_id);
    BTreeNode *node = (BTreeNode *)page_data;
    
    for (uint8_t i = 0; i < node->num_keys; i++) {
        
        if (!node->is_leaf) {
            btree_list_inorder(node->children[i], count);
        }

        if (node->is_leaf) {
            uart_print(" ");
            uart_print_dec16(node->keys[i]);
            uart_print("=");
            uart_print_dec16(node->values[i]);
                        (*count)++;
        }
    }

    if (!node->is_leaf) {
        btree_list_inorder(node->children[node->num_keys], count);
    }
}

static void db_list_all(void) {
    uart_println("=LIST=");
    uint8_t count = 0;
    btree_list_inorder(BTREE_ROOT_PAGE, &count);
    uart_print("N=");
    uart_print_dec16(count);
        uart_println("========");
}

static uint8_t db_find(uint8_t id, uint16_t *out_value) {
    return btree_search(id, out_value);
}

static uint8_t db_insert(uint8_t id, uint16_t value) {
    return btree_insert(id, value);
}

static uint8_t db_delete(uint8_t id) {
    return btree_delete(id);
}

static void db_stats(void) {
    uart_println("=STATS=");
    uart_print("Pg:");
    uart_print_dec16(PAGE_SIZE);
    uart_println(" bytes");
    
    uart_print("Data:");
    uart_print_dec16(DATA_PAGES);
    uart_print("/");
    uart_print_dec16(TOTAL_PAGES);
    uart_println("");
    
    uart_print("WAL:");
    uart_print_dec16(WAL_PAGE_ID);
    uart_println("");
    
    uart_print("Used:");
    uart_print_dec16(next_free_page);
        
    uart_print("t=");
    uart_print_dec16(BTREE_DEGREE);
        
    uart_print("Keys:");
    uart_print_dec16(BTREE_MAX_KEYS);
        
    uart_println("\n-WAL-");
    uart_print("Txn:");
    uart_println(wal_is_active() ? "YES" : "NO");
    uart_print("TxnID:");
    uart_print_dec16(wal_transaction_id);
        uart_print("Log:");
    uart_print_dec16(wal_log_slot);
    uart_print(" of ");
    uart_print_dec16(MAX_LOG_ENTRIES);
        
    uart_println("\n-Cache-");
    for (uint8_t i = 0; i < CACHE_SIZE; i++) {
        uart_print("Slot ");
        uart_print_dec16(i);
        uart_print(": ");
        if (cache.slots[i].valid) {
            uart_print("page=");
            uart_print_dec16(cache.slots[i].page_id);
            uart_print("d");
            uart_print_dec16(cache.slots[i].dirty);
            uart_print("l");
            uart_print_dec16(cache.slots[i].lru_counter);
        } else {
            uart_print("-");
        }
            }
    uart_println("===");
}

static uint8_t str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}

static uint16_t parse_uint16(const char *str, const char **end) {
    uint16_t result = 0;

    while (*str == ' ') str++;
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    if (end) *end = str;
    return result;
}

static void process_command(const char *cmd) {
    
    while (*cmd == ' ') cmd++;

    if (*cmd == '\0') return;
    
    if (str_starts_with(cmd, "INSERT ") || str_starts_with(cmd, "insert ")) {
        
        const char *ptr = cmd + 7;
        const char *end;
        
        uint16_t id = parse_uint16(ptr, &end);
        if (id > 255) {
            uart_println("ID 0-255");
            return;
        }
        
        uint16_t value = parse_uint16(end, NULL);
        
        db_insert((uint8_t)id, value);
    }
    else if (str_starts_with(cmd, "FIND ") || str_starts_with(cmd, "find ")) {
        
        const char *ptr = cmd + 5;
        uint16_t id = parse_uint16(ptr, NULL);
        
        if (id > 255) {
            uart_println("ID 0-255");
            return;
        }
        
        uint16_t value;
        if (db_find((uint8_t)id, &value)) {
            uart_print("R:");
            uart_print_dec16(id);
            uart_print("=");
            uart_print_dec16(value);
                    }
    }
    else if (str_starts_with(cmd, "DELETE ") || str_starts_with(cmd, "delete ")) {
        
        const char *ptr = cmd + 7;
        uint16_t id = parse_uint16(ptr, NULL);
        
        if (id > 255) {
            uart_println("ID 0-255");
            return;
        }
        
        db_delete((uint8_t)id);
    }
    else if (str_starts_with(cmd, "FLUSH") || str_starts_with(cmd, "flush")) {
        
        flush_cache();
    }
    else if (str_starts_with(cmd, "LIST") || str_starts_with(cmd, "list")) {
        
        db_list_all();
    }
    else if (str_starts_with(cmd, "STATS") || str_starts_with(cmd, "stats")) {
        
        db_stats();
    }
    else if (str_starts_with(cmd, "TREE") || str_starts_with(cmd, "tree")) {
        
        btree_print();
    }
    else if (str_starts_with(cmd, "INIT") || str_starts_with(cmd, "init")) {
        
        btree_init();
        uart_println("Init OK");
    }
    else if (str_starts_with(cmd, "HELP") || str_starts_with(cmd, "help") || 
             str_starts_with(cmd, "?")) {
        
                uart_println("=CMD=");
        uart_println("INSERT id val (id: 0-255)");
        uart_println("FIND id");
        uart_println("DEL id");
        uart_println("LIST");
        uart_println("FLUSH");
        uart_println("STATS");
        uart_println("TREE");
        uart_println("INIT");
        uart_println("RECOVER");
        uart_println("HELP");
                uart_println("Crash-safe");
        uart_println("======");
            }
    else if (str_starts_with(cmd, "RECOVER") || str_starts_with(cmd, "recover")) {
        
        db_recovery();
    }
    else {
        uart_print("? ");
        uart_println(cmd);
        uart_println("HELP");
    }
}

static void print_banner(void) {
            uart_println("SiliconDB 3.0");
    uart_println("KV Store");
    uart_println("+WAL");
    uart_println("ATmega328P");
            uart_print("ROM:");
    uart_print_dec16(EEPROM_SIZE);
    uart_println(" bytes");
    uart_print("Pg:");
    uart_print_dec16(DATA_PAGES);
    uart_print(" ");
    uart_print_dec16(DATA_PAGES * PAGE_SIZE);
    uart_println("B");
    uart_println("WAL@15");
    uart_print("Page Size: ");
    uart_print_dec16(PAGE_SIZE);
    uart_println(" bytes");
    uart_print("B-Tree Degree: ");
    uart_print_dec16(BTREE_DEGREE);
    uart_print("(");
    uart_print_dec16(BTREE_MAX_KEYS);
    uart_println("K)");
    uart_print("$:");
    uart_print_dec16(CACHE_SIZE);
    uart_println("p");
        uart_println("O(logN)+WAL");
    uart_println("HELP");
    }

int main(void) {
    
    uart_init();
    cache_init();

    db_recovery();

    btree_init();

    print_banner();

    uart_print("DB> ");

    while (1) {
        
        if (uart_rx_available()) {
            uint8_t c = uart_rx_byte();

            uart_tx_byte(c);

            if (c == '\r' || c == '\n') {
                
                uart_tx_byte('\n');
                cmd_buffer[cmd_index] = '\0';
                
                if (cmd_index > 0) {
                    process_command(cmd_buffer);
                }
                
                cmd_index = 0;
                uart_print("DB> ");
            }
            else if (c == '\b' || c == 127) {
                
                if (cmd_index > 0) {
                    cmd_index--;
                    uart_print(" \b");  
                }
            }
            else if (cmd_index < CMD_BUFFER_SIZE - 1) {
                
                cmd_buffer[cmd_index++] = c;
            }
        }

    }
    
    return 0;  
}
