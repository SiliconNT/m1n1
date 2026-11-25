
/* The host has handled an exception: hv_exc_proxy() */
void model_steal_time(u64 ticks)
{
    stolen_time += ticks;
}

u64 model_stolen_time(void)
{
    return stolen_time;
}

void model_exc_entry(void)
{
    hv_exc_entry();
}

void model_exc_exit(void)
{
    struct exc_info ctx = {0};

    hv_exc_exit(&ctx);
}

void model_exc_fiq(void)
{
    struct exc_info ctx = {0};

    hv_exc_fiq(&ctx);
}

void model_exc_proxy(int reason, u32 type)
{
    struct exc_info ctx = {0};

    hv_exc_proxy(&ctx, reason, type, NULL);
}

void model_cpu_off(void)
{
    struct exc_info ctx = {0};

    hv_cpu_off(&ctx);
}

/* What is kept of a core: something, as the guest may have left it; and whether it is all 0 */
void model_records_fill(int cpu)
{
    for (size_t i = 0; i < sizeof(pcpu[cpu]); i++)
        ((volatile u8 *)&pcpu[cpu])[i] = 0x5a;
}

bool model_records_clear(int cpu)
{
    for (size_t i = 0; i < sizeof(pcpu[cpu]); i++)
        if (((volatile u8 *)&pcpu[cpu])[i])
            return false;
    return true;
}
