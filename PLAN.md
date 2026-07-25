# Mich Core 1.5 — чистое гибридное ядро (2026-07-25)

Core 1.5 "boots itself": GRUB вышвырнут из репо насовсем - iso/, grub.cfg,
xorriso-пути стёрты; загрузка ТОЛЬКО через bigdevboot: src/bdb1.asm (stage1) +
src/bdb2.asm (stage2, те же бинарники что у MichOS), mkboot.py лепит boot-зону
256 блоков (blob @LBA32 = kflat-ядро, 0 модулей); make run = HDD -boot c,
ядро входит дверью BDBX ("Mich: bigdevboot protocol"); multiboot-ветка в
kernel.c сохранена как протокольная совместимость с деревом MichOS.

Core 1.4: двухпротокольная загрузка - свой BDBX (bigdevboot, BD_MAGIC 0x58424442,
bd_info/bd_module из src/bootinfo.h) + multiboot fallback для GRUB-ветки навсегда;
bd_info: mmap E820 по 24B, framebuffer (addr/pitch/w/h/bpp), до 8 модулей {start,end,cmdline}.

Core 1.3: шрифт Terminus 12x24 (src/font.h, генерится mkfont.py в полном дереве Mich),
gfx на метриках FONT_W/FONT_H, font_cps[] по кодпоинтам (готово к Unicode);
IPC: ipc_recv_from(expect, msg) - выборочный приём по отправителю (поле recv_expect),
ipc_flush_task(id) - централизованная чистка чужих wq при exit/kill (нет сирот в BLOCKED_SEND);
exec_gate - доставка отправителям заблокирована, пока получатель в kernel-side fs_fetch exec;
зеркало SYS_WRITE в serial (логи с -serial stdio); protos.h: IPC_STR 40, IPC_EOT 41.
Core 1.2: SYS_KILL (pid > 5, unlink из wq, страницы в PMM, код 137) под coreutils kill;
SYS_EXEC(path, args) - аргументы в стеке нового процесса (EBX);
SYS_WAIT pid = -2 = WNOHANG (реап-любого без гашения).
Core 1.1: wait-any (SYS_WAIT с pid = -1) + перенумерация id серверов.

Core 1.1: wait-any (SYS_WAIT с pid = -1 ждёт любого ребёнка, нужен init-реперу MichOS)
+ перенумерованные ID серверов в protos.h (kbd 1, ata 2, pci 3, fs 4, init 5).

Этот репозиторий = этап "ядро" проекта Mich, bigdevboss edition.
Сюда входят только механизмы ядра: boot, paging, PMM, задачи, планировщик,
синхронный IPC, сисколлы, fork/exec/exit/wait, ELF-загрузчик, консоль, serial.
Серверы юзерспейса (kbd/ata/pci/fs), шелл и программы живут в полном дереве Mich
и развиваются дальше в фазе MichOS поверх этого ядра.

Полный лог развития по этапам M1-M10 — ниже.

# Mich — дорожная карта

Принцип: ядро = механизмы (задачи, память, IPC, драйверы-инфраструктура),
ОС = политика (шелл, программы, VFS, init). Сейчас идёт ФАЗА ЯДРА.
shell.elf — тестовый жгут ядра, не часть будущей ОС.

## Фаза ядра

- [x] M1. multiboot2, GDT, IDT, paging (16MB identity)
- [x] M2. exceptions/panic, PIC, PIT-таймер, serial-lлог
- [x] M3. клавиатура (ps/2), сисколлы int 0x80, Ring 3, загрузка ELF из module2
- [x] M4. преемптивный round-robin планировщик (esp-swap по тику)
- [x] M5. IPC: send/recv через сисколл, блокирующий recv, состояния задач (running/blocked)
          синхронный rendezvous, сообщение 64 байта {from_id, type, data[56]};
          демо ping.elf/pong.elf — 5 обменов чисто, без фантомов
- [x] M6. драйверная модель юзерспейса: per-task iomap (TSS, биткарта 8K на задачу,
          подмена при свитче), SYS_IOPORT_ALLOW + SYS_IRQ_REG, доставка IRQ как IPC
          (from_id=0=ядро, коалесинг), SYS_SEND_NB (try-send для событий);
          keyboard.c ВЫНЕСЕН из ядра -> kbd.elf (первый сервер, shift поддержан),
          протокол = подписка + KBD_EVENT; бонус: SYS_TASKS + команда ps показывает
          живые состояния задач
- [x] M7. PMM по-взрослому + у каждого процесса СВОИ page tables
          bitmap по multiboot-mmap (128MB), alloc/free+zerofill,
          kernel-половина (0-16MB idmap, fb) = ОБЩИЕ supervisor PT для всех PD,
          user-половина с 0x1000000 = приватные PT на процесс;
          IPC-копии через kernel tmp + временный CR3-switch (xlat);
          ping.ld/pong.ld/kbd.ld СЛИТЫ в один user.ld - все на одном vaddr;
          5 CR3 в ротации (доказано -d int), free pages в ps;
          баг-улов: multiboot info надо резервировать в PMM (GRUB прячет его за kernel_end)
- [x] M8. ATA PIO драйвер (ata.elf, user-сервер), IDENTIFY + чтение секторов LBA28
          порты 0x1F0/0x3F6 через iomap, IRQ14 -> IPC; протокол ATA_READ_REQ/CHUNK
          (512 байт = 10 сообщений); shell команда `disk [lba]` с hexdump;
          model QEMU HARDDISK + sig 55AA подтверждены; запись - в M9 с ФС;
          улов: dash printf не знает \xNN (октальные коды), -boot d обязателен с диском
- [x] M8.5. PCI-скан в юзерспейсе (pci.elf через порты 0xCF8/0xCFC), lspci-таблица;
          7 устройств i440fx (host bridge, PIIX3/PIIX4, VGA, e1000 x2),
          multifunction по header type, class/vendor декод;
          run-цель теперь с e1000 на борту; зачищены последние asm-комментарии
- [x] M9. MichFS v1 + fs.elf (VFS/FS-сервер, id 7) + ATA write + mkfs.py;
          layout: sb@LBA0 "MICHFS01", bmap x5, imap, itable x256, 20217 блоков данных;
          инод 128B: type/mode/uid/gid/times/nlink + 12 direct + 1 indirect, файл до 70К;
          POSIX-штрихи: errno-имена (ENOENT/EISDIR/ENOSPC/...), "." и "..", NAME_MAX 27;
          fs - ни одного порта ввода-вывода, весь диск только через IPC к ata;
          ata: write 0x30 + flush 0xE7 (DRQ поллинг: QEMU не поднимает IRQ14 на write),
          запись принимает ТОЛЬКО от fs (from_id от ядра = неподделываемо);
          shell: motd на старте, ls, cat, echo > и >> (перезапись/допись), kq-кольцо ввода;
          ядро изменено на ОДНУ строку: MAX_TASKS 8->16; ulib.h для юзерспейса;
          улов босса: IPC-хиджак клиентских recv у серверов -> stash-архитектура;
          бонусные трупы: дроп shift-символов (жил с М6!), дроп клавиш в диалогах,
          инод-size-без-указателя (re-read клобберил память перед финальным flush);
          персистентность доказана: файл пережил перезагрузку свежего QEMU
- [x] M10. fork/exec/exit/wait (SYS 12-15 + GETPID 16) - честный fork как в 1969!
          fork: полная копия адресного пространства (клонируются user PT/PTE через
          scratch-окна 0x90000000/0x90001000 в ядерной половине), копия syscall-кадра
          (52 байта с вершины kstack: дитё просыпается из int 0x80 с eax=0);
          exec: ядро впервые стало IPC-КЛИЕНТОМ fs (стейджинг ELF с диска в буфер 70К,
          перекраска PD, патч iret-кадра - возврат прямо в новую программу);
          exit: успешная смерть -> zомби, юзер-страницы ВПЕРВЫЕ возвращены в PMM,
          CR3 уходит на kernel dir до освобождения; wait: блок-на-pid, реапинг
          (освобождает pd+kstack, слот FREE, повторное использование);
          pmm_alloc_page_low: ядерные структуры строго под 16MB (identity);
          разминка-баги закрыты: gfx-вывод атомарен (cli/sti вокруг SYS_WRITE/CLEAR),
          kbd: честные ринги 64 события на подписчика вместо блокирующего send
          из IRQ-петли; ulib обзавелся fork/exec/exit/wait/getpid;
          hello.elf запекается в MichFS (mkfs умеет бинари, мульти-блок до 12 direct);
          улов босса: scratch-PT резервировался ДО pmm_reserve модулей GRUB ->
          забирал страницу под ping.elf -> айди задач съезжали -> kbd шёл в ata;
          лечится одной перестановкой вызова; труп соседний: fork не ставил c->id
          (rc=0 у родителя -> шелл exec'ил сам себя и умирал 42);
          доказательный стенд: run hello = pid8 + pid9 + коды 42/7, free pages
          до/после = 32374/32374 (ноль утечек), слот pid8 переиспользуется
- [ ] опционально: kmalloc в ядре, named pipes, сигналы, CoW-fork (M10.5)

## Фаза ОС (после M10)

- init-процесс (первый из ФС), настоящий шелл со скриптами и пайпами
- утилиты: ls, cat, echo, ps, kill
- мини-libc для user-программ (crt0, string, print)
- протоколы серверов: реестр имён (как процесс находит VFS по имени "vfs")

## HW-пакет (после фазы ОС, курс bigdevboss "сначала железо")

- AHCI/SATA или virtio-blk вместо PIO (потребитель уже есть - ФС)
- APIC/IOAPIC + HPET вместо PIC/PIT
- e1000/rtl8139/virtio-net + TCP/IP стек (пинг Mich снаружи!)
- bare metal: загрузка на реальном компе с флешки

## S-этап (безопасность, после фазы ОС)

- валидация юзер-указателей в сисколлах, границы id/count, double-free PMM
- очередь событий kbd вместо drop; шелл-баги (text>, гонка консоли)
- SMEP/SMAP/NX-эквиваленты в мапах, аудит xlat-путей

## Решённые вопросы

1. IPC: СИНХРОННЫЙ rendezvous (M5) — плюнем в сторону L4/Mach.
2. Сообщение фиксированное, 64 байта: {from_id, type, data[56]}.
3. Порты: PER-TASK iomap в TSS, подмена биткарты на свитче (он решил по-взрослому).
4. Клава: СОБЫТИЯ по подписке (KBD_EVENT через try-send), не request/reply.
5. Очерёдность: строго M6->M7->M8->M9->M10, без перестановок (делегировано мне).

## Открытые вопросы (решим по ходу)

6. Как серверы регистрируются по имени: отдельный registry или вшитые id?
   (пока вшитые: id = порядок module2 в grub.cfg; registry — в фазе ОС)
