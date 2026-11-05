// SDK-owned removal also covers subagent transcripts. A resumed session can have copies in
// several project directories, so repeat its global lookup until that id is absent.
export async function deleteSessionCopies(deleteSession, threadId) {
  if (typeof deleteSession !== "function" || typeof threadId !== "string" || !threadId)
    throw new Error("This Claude SDK cannot delete the requested session.");
  for (let copies = 0; copies < 256; ++copies) {
    try { await deleteSession(threadId); }
    catch (error) {
      if (error?.code === "ENOENT" || (String(error?.message).includes(threadId) && /not found/i.test(error.message)))
        return;
      throw error;
    }
  }
  throw new Error("Session cleanup has more copies to remove; retry the request.");
}
