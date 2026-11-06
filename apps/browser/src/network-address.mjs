import dns from "node:dns/promises";
import net from "node:net";

// Classifies the address a proxied hop actually connects to. Hostname checks
// alone let a public name resolve to a private, link-local or metadata address.

function blockList(ranges) {
  const list = new net.BlockList();
  for (const [address, prefix, family] of ranges) list.addSubnet(address, prefix, family);
  return list;
}

const LOOPBACK = blockList([
  ["127.0.0.0", 8, "ipv4"],
  ["::1", 128, "ipv6"],
]);

const PRIVATE = blockList([
  ["10.0.0.0", 8, "ipv4"],
  ["172.16.0.0", 12, "ipv4"],
  ["192.168.0.0", 16, "ipv4"],
  ["100.64.0.0", 10, "ipv4"], // carrier-grade NAT, also Tailscale
  ["fc00::", 7, "ipv6"],
]);

const LINK_LOCAL = blockList([
  ["169.254.0.0", 16, "ipv4"], // includes cloud metadata 169.254.169.254
  ["fe80::", 10, "ipv6"],
]);

const RESERVED = blockList([
  ["0.0.0.0", 8, "ipv4"],
  ["192.0.0.0", 24, "ipv4"],
  ["198.18.0.0", 15, "ipv4"],
  ["224.0.0.0", 4, "ipv4"],
  ["240.0.0.0", 4, "ipv4"],
  ["::", 128, "ipv6"],
  ["ff00::", 8, "ipv6"],
]);

// "loopback", "private", "link-local", "reserved" or "public"; "invalid" for a non-IP.
export function addressKind(address) {
  let value = String(address ?? "").replace(/^\[|\]$/g, "");
  // IPv4-mapped IPv6 (::ffff:10.0.0.1) reaches the IPv4 address.
  const mapped = /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i.exec(value);
  if (mapped) value = mapped[1];
  const family = net.isIP(value);
  if (!family) return "invalid";
  const type = family === 4 ? "ipv4" : "ipv6";
  if (LOOPBACK.check(value, type)) return "loopback";
  if (LINK_LOCAL.check(value, type)) return "link-local";
  if (PRIVATE.check(value, type)) return "private";
  if (RESERVED.check(value, type)) return "reserved";
  return "public";
}

// Every address the host resolves to, in resolver order. An IP literal resolves to itself.
export async function resolveAddresses(hostname, lookup = dns.lookup) {
  const host = String(hostname).replace(/^\[|\]$/g, "");
  if (net.isIP(host)) return [host];
  const results = await lookup(host, { all: true, verbatim: true });
  return results.map((result) => result.address);
}
