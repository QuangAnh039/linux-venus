cmd_drivers/char/diag/modules.order := {   echo drivers/char/diag/diagchar.ko; :; } | awk '!x[$$0]++' - > drivers/char/diag/modules.order
