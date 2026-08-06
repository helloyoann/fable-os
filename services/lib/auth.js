// Passwords, sessions, and login throttling.
//
// Passwords are scrypt-hashed with a per-user random salt and compared in
// constant time. Sessions are opaque random tokens stored server-side with a
// per-session CSRF token; the browser only ever holds the token in an
// HttpOnly cookie.

import { scryptSync, randomBytes, timingSafeEqual } from 'node:crypto';

const SESSION_TTL_MS = 30 * 24 * 60 * 60 * 1000; // 30 days

export function hashPassword(password) {
  const salt = randomBytes(16);
  const hash = scryptSync(password, salt, 32);
  return `scrypt:${salt.toString('hex')}:${hash.toString('hex')}`;
}

export function verifyPassword(password, stored) {
  const [scheme, saltHex, hashHex] = String(stored ?? '').split(':');
  if (scheme !== 'scrypt' || !saltHex || !hashHex) return false;
  const hash = scryptSync(password, Buffer.from(saltHex, 'hex'), 32);
  const expected = Buffer.from(hashHex, 'hex');
  return hash.length === expected.length && timingSafeEqual(hash, expected);
}

export function createSession(store, userId) {
  return store.collection('sessions').insert({
    token: randomBytes(32).toString('hex'),
    csrf: randomBytes(16).toString('hex'),
    user_id: userId,
    expires_at: new Date(Date.now() + SESSION_TTL_MS).toISOString(),
  });
}

export function getSession(store, token) {
  if (!token) return null;
  const sessions = store.collection('sessions');
  const session = sessions.findOne((s) => s.token === token);
  if (!session) return null;
  if (session.expires_at < new Date().toISOString()) {
    sessions.remove(session.id);
    return null;
  }
  return session;
}

export function destroySession(store, token) {
  const session = store.collection('sessions').findOne((s) => s.token === token);
  if (session) store.collection('sessions').remove(session.id);
}

// Login throttle: 10 failed attempts per email+IP per 15 minutes. In-memory
// on purpose — a restart clearing the counters is acceptable, persistence of
// abuse counters is not worth a schema.
const WINDOW_MS = 15 * 60 * 1000;
const MAX_ATTEMPTS = 10;
const attempts = new Map();

export function loginAllowed(key) {
  const entry = attempts.get(key);
  if (!entry || entry.resetAt < Date.now()) return true;
  return entry.count < MAX_ATTEMPTS;
}

export function loginFailed(key) {
  const entry = attempts.get(key);
  if (!entry || entry.resetAt < Date.now()) {
    attempts.set(key, { count: 1, resetAt: Date.now() + WINDOW_MS });
  } else {
    entry.count += 1;
  }
}

export function loginSucceeded(key) {
  attempts.delete(key);
}
