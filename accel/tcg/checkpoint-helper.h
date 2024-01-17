#ifdef CONFIG_CHECKPOINT
DEF_HELPER_FLAGS_2(checkpoint_sync_check, TCG_CALL_NO_RWG , void, i32, ptr)
#endif
