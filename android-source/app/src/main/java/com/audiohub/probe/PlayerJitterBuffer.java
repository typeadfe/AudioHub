package com.audiohub.probe;

/**
 * 单源抖动缓冲。
 *
 * 【职责】
 *   1. 【预热】先攒够目标水位再开始播，否则网络抖动会直接变成断音。
 *   2. 按序号放进环形槽位，按 nextSeq 顺序取帧
 *   3. 【稳态水位调节】水位过高就丢旧帧；目标调大时重新预热
 *
 * ==================================================================
 * 【为什么存"不透明负载"而不是 PCM 样本】
 * 音频是 PCM。抖动缓冲只负责【按序号重排】，
 * 不该关心里面装的是什么，因此一律按 byte[] 存取，解码交给调用方。
 *
 * 【为什么水位调节必须跟着设置项走】
 * 早期实现把高低水位写成【固定常量】（16 帧 / 4 帧），而设置里的
 * "音频质量与延迟"只影响预热帧数。结果是：预热完之后无论选 20ms 还是 80ms，
 * 稳态水位都由那两个常量决定，用户感觉"这个选项没有用"。
 * 现在高低水位【由目标值推导】，设置项才真正控制稳态水位。
 *
 * 【帧长换算的坑】
 * 目标值必须以【毫秒】为准再换算成本路帧数。
 * ==================================================================
 */
final class PlayerJitterBuffer {

    private static final int CAP = 64;

    /** 目标水位（毫秒），由设置里的「音频质量与延迟」控制，全局共享 */
    private static volatile int g_targetMs = Protocol.JITTER_TARGET_MS;
    private volatile int targetMsOverride = -1;

    /** 设置目标水位（毫秒）。会夹到 10~200ms */
    static void setTargetMs(int ms) {
        if (ms < 10) ms = 10;
        if (ms > 200) ms = 200;
        g_targetMs = ms;
    }

    static int targetMs() { return g_targetMs; }

    synchronized void setTargetMsInstance(int ms) {
        targetMsOverride = (ms < 10 || ms > 200) ? -1 : ms;
        warmedUp = false;
        warmWait = 0;
    }

    /** 本路每帧时长（毫秒） */
    private final int frameMs;
    private final byte[][] slots = new byte[CAP][];
    private final boolean[] present = new boolean[CAP];

    private boolean started = false;
    private boolean warmedUp = false;
    private boolean haveHighest = false;
    private int  nextSeq = 0;
    private int  highestSeq = 0;
    private int  warmWait = 0;
    private int  appliedTargetFrames = 0;

    // 统计
    long recvFrames = 0, lostFrames = 0, dupFrames = 0;
    long droppedFrames = 0, lateFrames = 0, underruns = 0;

    PlayerJitterBuffer(int frameMs) {
        this.frameMs = (frameMs > 0) ? frameMs : Protocol.FRAME_MS;
    }

    // ------------------------------------------------------------------
    /**
     * 本路实际使用的目标水位（毫秒）。
     *
     */
    private int effectiveTargetMs() {
        return targetMsOverride > 0 ? targetMsOverride : g_targetMs;
    }

    /** 目标帧数（按本路真实帧长换算，而不是固定按 5ms） */
    private int targetFrames() {
        int f = effectiveTargetMs() / frameMs;
        if (f < 1) f = 1;
        if (f > CAP / 2) f = CAP / 2;
        return f;
    }

    /** 高水位：高于它就开始丢帧追赶 */
    private int highFrames() {
        int h = targetFrames() + 2;
        if (h > CAP - 2) h = CAP - 2;
        return h;
    }

    synchronized void reset() {
        for (int i = 0; i < CAP; i++) { present[i] = false; slots[i] = null; }
        started = false; warmedUp = false; haveHighest = false;
        nextSeq = 0; highestSeq = 0; warmWait = 0; appliedTargetFrames = 0;
        recvFrames = lostFrames = dupFrames = droppedFrames = lateFrames = underruns = 0;
    }

    synchronized boolean warmedUp() { return warmedUp; }

    private static int seqDiff(int a, int b) {
        return (short) (a - b);
    }

    synchronized void push(int seq, byte[] payload) {
        if (payload == null) return;
        recvFrames++;
        seq &= 0xFFFF;

        if (!started) { started = true; nextSeq = seq; }
        if (!haveHighest) { highestSeq = seq; haveHighest = true; }
        else if (seqDiff(seq, highestSeq) > 0) highestSeq = seq;

        if (seqDiff(seq, nextSeq) < 0) { lateFrames++; return; }
        if (seqDiff(seq, nextSeq) >= CAP) {
            // A long transport stall has moved the sender beyond our ring.
            // Resynchronize instead of rejecting every future packet forever.
            for (int i = 0; i < CAP; i++) { present[i] = false; slots[i] = null; }
            nextSeq = seq;
            highestSeq = seq;
            warmedUp = false;
            warmWait = 0;
            droppedFrames++;
        }

        int idx = seq % CAP;
        if (present[idx]) dupFrames++;
        present[idx] = true;
        slots[idx] = payload;
    }

    synchronized int pending() {
        if (!haveHighest) return 0;
        int d = seqDiff(highestSeq, nextSeq) + 1;
        return d > 0 ? d : 0;
    }

    /** 待播时长（毫秒），按本路真实帧长折算 */
    synchronized int pendingMs() { return pending() * frameMs; }

    /**
     * 取下一帧负载。
     * 返回 null 表示此刻应输出静音；只返回实际收到的帧。
     * 
     */
    synchronized byte[] pop() {
        if (!started) return null;

        int target = targetFrames();
        if (target != appliedTargetFrames) {
            // Raising the quality target must really build the new buffer depth.
            // Lowering it is handled by the catch-up loop below.
            if (target > appliedTargetFrames && pending() < target) warmedUp = false;
            appliedTargetFrames = target;
            warmWait = 0;
        }

        if (!warmedUp) {
            if (pending() >= target) {
                warmedUp = true;
            } else if (++warmWait > 400) {
                warmedUp = true;               // 源太慢，不再无限等
            } else {
                return null;
            }
        }

        int buffered = pending();
        if (buffered <= 0) {
            underruns++;
            return null;
        }

        // 水位过高：丢一帧往前追。
        // 阈值来自 targetFrames()，所以"稳定优先"会让水位稳定在更高处，
        // "低延迟"则主动丢弃多余帧把水位压下来 —— 设置项这才真正生效。
        while (buffered > highFrames()) {
            present[nextSeq % CAP] = false;
            slots[nextSeq % CAP] = null;
            nextSeq = (nextSeq + 1) & 0xFFFF;
            droppedFrames++;
            buffered--;
        }

        int idx = nextSeq % CAP;
        byte[] payload = present[idx] ? slots[idx] : null;
        if (payload != null) {
            present[idx] = false;
            slots[idx] = null;
        } else {
            lostFrames++;
        }
        nextSeq = (nextSeq + 1) & 0xFFFF;
        return payload;
    }
}
