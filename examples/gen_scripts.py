import random

def generate_events(filename, num_events, price_min, price_max):
    active_ids = []
    next_id = 1
    
    with open(filename, 'w') as f:
        for _ in range(num_events):
            if not active_ids or random.random() < 0.8:
                side = random.choice(['B', 'S'])
                price = random.randint(price_min, price_max)
                qty = random.randint(1, 100)
                f.write(f"add {next_id} {side} {price} {qty}\n")
                active_ids.append(next_id)
                next_id += 1
            else:
                cancel_idx = random.randrange(len(active_ids))
                cancel_id = active_ids.pop(cancel_idx)
                f.write(f"cancel {cancel_id}\n")

if __name__ == '__main__':
    generate_events("dense_script.txt", 50000, 499950, 500050)
    generate_events("deep_script.txt", 50000, 400000, 600000)