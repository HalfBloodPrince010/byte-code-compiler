#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table *table) {
  table->count = 0;
  table->capacity = 0;
  table->entries = NULL;
}

void freeTable(Table *table) {
  FREE_ARRAY(Table, table->entries, table->capacity);
  initTable(table);
}

static Entry *findEntry(Entry *entries, int capacity, ObjString *key) {
  // HashFunction converts string to an integer, below Modulo ensures we map
  // that to a bucket. We could have even used a ASCII value of last char, or
  // sum of ASCII of each char then taken Modulo with Array Size or Capacity to
  // map it to one of the bucket. Current Hash Function ensures uniformity.
  uint32_t index = key->hash % capacity;
  Entry *tombstone = NULL;

  // Linear Probing
  for (;;) {
    Entry *entry = &entries[index];
    // We are comparing 2 String Objects, not exactly string "chars".
    if (entry->key == NULL) {
      if (IS_NIL(entry->value)) {
        // Empty Entry.
        return tombstone != NULL ? tombstone : entry;
      } else {
        // value is Bool:True, so we have found a Tombstone
        if (tombstone == NULL) {
          tombstone = entry;
        }
      }
    } else if (entry->key == key) {
      // We found the key
      return entry;
    }

    // Modulo to wrap around if we are at the last element.
    index = (index + 1) % capacity;
  }
}

bool tableGet(Table *table, ObjString *key, Value *value) {
  if (table->count == 0)
    return false;

  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL) {
    return false;
  }

  *value = entry->value;
  return true;
}

static void adjustCapacity(Table *table, int capacity) {
  Entry *entries = ALLOCATE(Entry, capacity);
  for (int i = 0; i < capacity; i++) {
    entries[i].key = NULL;
    entries[i].value = NIL_VAL;
  }

  // Unlike regular case where we copy all elements to bigger heap allocated
  // memory when we Realloc, we can't do it her because the "index" we use is
  // obtained from taking modulo with array length or capacity is updated here.
  // Hence we need to re-calculate all the entries and probe sequence for each
  // key present.
  table->count = 0; // Don't count the Tombstone value.
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (entry->key == NULL)
      continue;

    Entry *destEntry = findEntry(entries, capacity, entry->key);
    destEntry->key = entry->key;
    destEntry->value = entry->value;
    table->count++;
  }

  FREE_ARRAY(Entry, table->entries, table->capacity);
  table->entries = entries;
  table->capacity = capacity;
}

bool tableSet(Table *table, ObjString *key, Value value) {
  if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
    int capacity = GROW_CAPACITY(table->capacity);
    adjustCapacity(table, capacity);
  }
  Entry *entry = findEntry(table->entries, table->capacity, key);
  bool isNewKey = entry->key == NULL;
  // Count only if its new, and not a tombstone
  if (isNewKey && IS_NIL(entry->value)) {
    table->count++;
  }

  entry->key = key;
  entry->value = value;
  return isNewKey;
}

bool tableDelete(Table *table, ObjString *key) {
  if (table->count == 0) {
    return false;
  }

  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL) {
    return false;
  }

  // Place a tombstone in the entry;
  entry->key = NULL;
  entry->value = BOOL_VAL(true);
  return true;
}

void tableAddAll(Table *from, Table *to) {
  for (int i = 0; i < from->capacity; i++) {
    Entry *entry = &from->entries[i];
    if (entry->key != NULL) {
      tableSet(to, entry->key, entry->value);
    }
  }
}

ObjString *tableFindString(Table *table, const char *chars, int length,
                           uint32_t hash) {
  if (table->count == 0) {
    return NULL;
  }

  uint32_t index = hash % table->capacity;
  // we grow the table capacity when we are at the LOAD factor of 0.75, this
  // will ensure there is always empty entry in the entries to avoid infinite
  // loops.
  for (;;) {
    Entry *entry = &table->entries[index];
    if (entry->key == NULL) {
      // stop if we empty entry, which is not a tombstone entry.
      if (IS_NIL(entry->value)) {
        return NULL;
      }
    } else if (entry->key->length == length && entry->key->hash == hash &&
               memcmp(entry->key->chars, chars, length) == 0) {
      return entry->key;
    }

    index = (index + 1) % table->capacity;
  }
}

void markTable(Table *table) {
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    markObject((Obj *)entry->key);
    markValue(entry->value);
  }
}
