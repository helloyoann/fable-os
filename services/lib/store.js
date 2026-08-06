// Persistence for the services platform.
//
// One JSON file per collection, rewritten atomically (temp file + rename) on
// every mutation. At the network's scale — thousands of members, tens of
// thousands of jobs — this is well within what a single process handles, and
// it keeps the platform dependency-free: `node server.js` runs anywhere.
// If write volume ever demands it, swap SQLite or Postgres in behind this
// same Collection interface; nothing above this file knows about files.

import { mkdirSync, readFileSync, writeFileSync, renameSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';

export class Store {
  constructor(dir) {
    this.dir = dir;
    mkdirSync(dir, { recursive: true });
    this._collections = new Map();
  }

  collection(name) {
    if (!/^[a-z_]+$/.test(name)) throw new Error(`bad collection name: ${name}`);
    if (!this._collections.has(name)) {
      this._collections.set(name, new Collection(join(this.dir, `${name}.json`)));
    }
    return this._collections.get(name);
  }
}

class Collection {
  constructor(path) {
    this.path = path;
    this.rows = existsSync(path) ? JSON.parse(readFileSync(path, 'utf8')) : [];
  }

  _save() {
    // Write-then-rename so a crash mid-write never truncates the live file.
    const tmp = `${this.path}.tmp`;
    writeFileSync(tmp, JSON.stringify(this.rows));
    renameSync(tmp, this.path);
  }

  all() {
    return [...this.rows];
  }

  get(id) {
    return this.rows.find((r) => r.id === id) ?? null;
  }

  find(fn) {
    return this.rows.filter(fn);
  }

  findOne(fn) {
    return this.rows.find(fn) ?? null;
  }

  insert(doc) {
    const row = { id: randomUUID(), created_at: new Date().toISOString(), ...doc };
    this.rows.push(row);
    this._save();
    return row;
  }

  update(id, patch) {
    const row = this.get(id);
    if (!row) return null;
    Object.assign(row, patch, { updated_at: new Date().toISOString() });
    this._save();
    return row;
  }

  remove(id) {
    const i = this.rows.findIndex((r) => r.id === id);
    if (i === -1) return false;
    this.rows.splice(i, 1);
    this._save();
    return true;
  }
}
