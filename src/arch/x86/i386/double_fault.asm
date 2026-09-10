global df_task_entry
extern panic_df

df_task_entry:
    cli
    cld
    call panic_df
hang:
    hlt
    jmp hang

section .note.GNU-stack noalloc noexec nowrite progbits
