#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/sched.h>

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <asm/div64.h>

//--------------------------------------------------------------------------------------------------------

MODULE_AUTHOR("Bruno, Emanuel e Thiago");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSTF IO scheduler parametrizável para Linux 4.13");

//--------------------------------------------------------------------------------------------------------
static int queue_depth = 50;
module_param(queue_depth, int, 0444);
MODULE_PARM_DESC(queue_depth, "Número de requisições na fila antes do despacho (20-100)");

static int max_wait_ms = 5000;
module_param(max_wait_ms, int, 0444);
MODULE_PARM_DESC(max_wait_ms, "Tempo máximo de espera em ms antes do despacho (20-5000)");

static bool debug = true;
module_param(debug, bool, 0444);
MODULE_PARM_DESC(debug, "Habilita mensagens de depuração no log do kernel");
//-------------------------------------------------------------------------------------------------------------------------------
/// ESTRUTURAS 
struct disk_data_s {
	sector_t head_last_pos;
	sector_t head_pos;
	char head_dir;
};

struct sstf_data_s {
	struct list_head queue;
	int depth;
	struct timer_list flush_timer;
	struct request_queue *q;
	spinlock_t lock;  
	struct disk_data_s disk_data;
};

//-------------------------------------------------------------------------------------------------------------------------------

//Funções principais 
static int sstf_init_queue(struct request_queue *q, struct elevator_type *e);
static void sstf_exit_queue(struct elevator_queue *e);
static int sstf_dispatch(struct request_queue *q, int force);
static void sstf_add_request(struct request_queue *q, struct request *rq);
static void sstf_merged_requests(struct request_queue *q, struct request *rq, struct request *next);
//---------------------------------------------------------------------------------------------------

// Funções auxiliares
static void sstf_flush_requests(struct sstf_data_s *nd);
static void sstf_timer_fn(unsigned long data);
//-------------------------------------------------------------------------------------------------------------------------------

/// Define a estrutura principal que descreve o escalonador de I/O "sstf"
static struct elevator_type elevator_sstf = {
	.ops.sq = {		
		.elevator_merge_req_fn = sstf_merged_requests,// Função chamada quando duas requisições contíguas podem ser unificadas		
		.elevator_dispatch_fn = sstf_dispatch,        // Função que seleciona a próxima requisição a ser despachada ao driver de bloco		
		.elevator_add_req_fn = sstf_add_request,      // Função chamada quando uma nova requisição de I/O é adicionada à fila		
		.elevator_init_fn = sstf_init_queue,          // Função que inicializa o escalonador para uma fila de requisição		
		.elevator_exit_fn = sstf_exit_queue,          // Função chamada quando o escalonador está sendo removido
	},
	.elevator_name = "sstf", // Nome que identifica o escalonador no sistema (ex.: "sstf")
	.elevator_owner = THIS_MODULE, // Indica que este escalonador pertence ao módulo atual
};

/// Função chamada quando o módulo do kernel é carregado
static int __init sstf_init(void){
	/// Imprime uma mensagem no log indicando a inicialização do escalonador
	pr_info("\nSSTF scheduler init (queue_depth=%d, max_wait=%dms, debug=%s)\n",queue_depth, max_wait_ms, debug ? "on" : "off");
	/// Registra o escalonador no subsistema de I/O do kernel
	return elv_register(&elevator_sstf);
}
/// Função chamada quando o módulo do kernel é descarregado
static void __exit sstf_exit(void){
	/// Informa no log que o escalonador está sendo removido
	pr_info("\nSSTF scheduler exit\n");
	/// Remove o escalonador do subsistema de I/O
	elv_unregister(&elevator_sstf);
}
/*------------------------------------------------------------------------------------------------------
*                                             <Funções auxiliares>
*------------------------------------------------------------------------------------------------------*/

///------------------------------------------------------------------------------------------------> FLUSH:
/// Despacha todas as requisições pendentes da fila SSTF associada a `nd`.
/// 
/// IMPORTANTE: Esta função assume o controle do spinlock `nd->lock`,
/// e o solta temporariamente a cada iteração para evitar deadlock com `sstf_dispatch()`,
/// que também utiliza esse mesmo lock internamente.
///
/// Estratégia:
/// - Enquanto houver requisições na fila (`nd->queue`), solta o lock e chama `sstf_dispatch()`
/// - Após cada despacho, retoma o lock para checar novamente a fila
/// - Isso garante exclusão mútua sem manter o lock durante operações de longa duração
static void sstf_flush_requests(struct sstf_data_s *nd) {
	unsigned long flags;
	spin_lock_irqsave(&nd->lock, flags);
	while (!list_empty(&nd->queue)) {
		spin_unlock_irqrestore(&nd->lock, flags);
		sstf_dispatch(nd->q, 1);
		spin_lock_irqsave(&nd->lock, flags);
	}
	spin_unlock_irqrestore(&nd->lock, flags);
	// nd->depth = 0;
}

///------------------------------------------------------------------------------------------------> TIMER:
/// -- Callback do temporizador (`timer_list`) do SSTF.
/// -Este timer é ativado para garantir que requisições na fila não fiquem esperando
/// indefinidamente mesmo se a profundidade da fila (`nd->depth`) não for atingida.
/// -Ao expirar, ele chama `sstf_flush_requests()` para forçar o despacho.
/// -O timer é reiniciado em `sstf_add_request()` sempre que novas requisições chegam.
static void sstf_timer_fn(unsigned long data){
	struct sstf_data_s *nd = (struct sstf_data_s *)data;

	/// DEBUG:
	if (unlikely(debug)) printk(KERN_DEBUG "[TIMER SSTF] timer expirado, despacho de %d requisições\n", nd->depth);

	if (!list_empty(&nd->queue)) sstf_flush_requests(nd);
}

/*------------------------------------------------------------------------------------------------------
*                                <Implementação das funções principais>
*------------------------------------------------------------------------------------------------------*/

///------------------------------------------------------------------------------------------------> INIT:
static int sstf_init_queue(struct request_queue *q, struct elevator_type *e){
	struct sstf_data_s *nd;
	struct elevator_queue *eq;

	// Aloca a estrutura base do escalonador (com funções e tipo)
	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	// Aloca e inicializa os dados internos do SSTF
	nd = kmalloc_node(sizeof(*nd), GFP_KERNEL, q->node);
	if (!nd) {
		kobject_put(&eq->kobj); // desfaz alocação anterior
		return -ENOMEM;
	}

	memset(nd, 0, sizeof(*nd)); // segurança defensiva (opcional)
	spin_lock_init(&nd->lock);
	INIT_LIST_HEAD(&nd->queue);
	nd->depth = 0;
	nd->q = q;

	// Inicializa o estado do disco local (antes era global)
	nd->disk_data.head_last_pos = 1;
	nd->disk_data.head_pos = 1;
	nd->disk_data.head_dir = 'P';

	// Inicializa o timer com handler sstf_timer_fn
	//timer_setup(&nd->flush_timer, sstf_timer_fn, 0);  //4.15

	init_timer(&nd->flush_timer); //4.13
	nd->flush_timer.function = sstf_timer_fn;
	nd->flush_timer.data = (unsigned long)nd;

	// Associa os dados do SSTF à estrutura do elevador
	eq->elevator_data = nd;

	// Protege a alteração da fila principal com spinlock
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

///------------------------------------------------------------------------------------------------> EXIT:
static void sstf_exit_queue(struct elevator_queue *e){
	struct sstf_data_s *nd = e->elevator_data;
	del_timer_sync(&nd->flush_timer);
	WARN_ON(!list_empty(&nd->queue));
	kfree(nd);
}

///------------------------------------------------------------------------------------------------> ADD:
static void sstf_add_request(struct request_queue *q, struct request *rq) {
	// teste de erro ----------------------------------------------------------
	if (!rq) return; 
	//-------------------------------------------------------------------------
	struct sstf_data_s *nd = q->elevator->elevator_data;
	char operation;
	u64 now;
	unsigned long flags;

	spin_lock_irqsave(&nd->lock, flags);
	//-------------------------------------------------------------------------
	
	mod_timer(&nd->flush_timer, jiffies + msecs_to_jiffies(max_wait_ms));
	list_add_tail(&rq->queuelist, &nd->queue);
	nd->depth++;
	
	/// DEBUG:
	if (unlikely(debug)) {
		now = ktime_get_ns();
		do_div(now, 1000000);
		operation = rq_data_dir(rq) == READ ? 'R' : 'W';
		printk(KERN_DEBUG "[SSTF] add %c sec %llu (%llums) depth=%d",
			operation, (unsigned long long)blk_rq_pos(rq),
			(unsigned long long)now, nd->depth);
	}

	if (nd->depth >= queue_depth) {
		mod_timer(&nd->flush_timer, jiffies + msecs_to_jiffies(max_wait_ms*10));
		if (unlikely(debug)) printk(KERN_DEBUG "[ADD SSTF] depth reached (%d), flush now\n", nd->depth);
		spin_unlock_irqrestore(&nd->lock, flags);
		sstf_flush_requests(nd);
		return;
	}
	//-------------------------------------------------------------------------
	spin_unlock_irqrestore(&nd->lock, flags);
}
///------------------------------------------------------------------------------------------------> DISPATCH:
static int sstf_dispatch(struct request_queue *q, int force) {
	struct sstf_data_s *nd = q->elevator->elevator_data;
	struct request *rq, *best = NULL;
	sector_t pos, best_pos = 0;
	unsigned long long dist, best_dist = ULLONG_MAX;
	char operation;
	u64 now;
	unsigned long flags;
	
	if (!force && nd->depth < queue_depth)
        return 0;
		
	spin_lock_irqsave(&nd->lock, flags);
	//------------------------------------------------------------------------------------
	// Inicializa posição da cabeça se necessário
	if (nd->disk_data.head_pos == (sector_t)-1 && !list_empty(&nd->queue)) {
		rq = list_first_entry(&nd->queue, struct request, queuelist);
		nd->disk_data.head_pos = blk_rq_pos(rq);
	}

	// Procura a requisição mais próxima da posição atual
	list_for_each_entry(rq, &nd->queue, queuelist) {
		pos = blk_rq_pos(rq);
		dist = (pos > nd->disk_data.head_pos) ? (pos - nd->disk_data.head_pos)
		                                      : (nd->disk_data.head_pos - pos);
		if (dist < best_dist) {
			best_dist = dist;
			best = rq;
			best_pos = pos;
		}
	}

	if (!best) {
		spin_unlock_irqrestore(&nd->lock, flags);
		return 0;
	}

	// Remove a requisição escolhida da fila
	list_del_init(&best->queuelist);

	// Atualiza estado da cabeça
	nd->disk_data.head_last_pos = nd->disk_data.head_pos;
	nd->disk_data.head_pos = best_pos;
	nd->disk_data.head_dir = (nd->disk_data.head_pos >= nd->disk_data.head_last_pos) ? 'R' : 'L';

	nd->depth--;
	//------------------------------------------------------------------------------------
	spin_unlock_irqrestore(&nd->lock, flags);

	// Despacha a requisição para o driver de bloco
	elv_dispatch_sort(q, best);

	/// DEBUG:
	if (unlikely(debug)) {
		now = ktime_get_ns();
		do_div(now, 1000000);  // nanos → millis
		operation = rq_data_dir(best) == READ ? 'R' : 'W';
		printk(KERN_DEBUG "[SSTF] dsp %c sec %llu (%llums) head:%llu dir:%c\n",
			operation,
			(unsigned long long)best_pos,
			(unsigned long long)now,
			(unsigned long long)nd->disk_data.head_pos,
			nd->disk_data.head_dir);
	}

	return 1;
}

///------------------------------------------------------------------------------------------------> MERGE:
static void sstf_merged_requests(struct request_queue *q, struct request *rq, struct request *next){
	struct sstf_data_s *nd = q->elevator->elevator_data;
	unsigned long flags;

	spin_lock_irqsave(&nd->lock, flags);
	list_del_init(&next->queuelist);
	spin_unlock_irqrestore(&nd->lock, flags);
}

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------


/// Declara que sstf_init deve ser chamado na inicialização do módulo
module_init(sstf_init);

/// Declara que sstf_exit deve ser chamado na remoção do módulo
module_exit(sstf_exit);


