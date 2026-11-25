
void model_exc_entry(void);
void model_exc_exit(void);

void model_exc_entry(void)
{
    hv_exc_entry();
}

void model_exc_exit(void)
{
    struct exc_info ctx = {0};

    hv_exc_exit(&ctx);
}
