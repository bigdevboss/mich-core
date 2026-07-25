global double_fault_handler
extern panic_df

double_fault_handler:
    pusha
    call panic_df
    popa
    iret
