/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *                2024  Akash Levy        <akash@silimate.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct SplitfanoutWorker
{
	Module *module;
	SigMap sigmap;
	dict<SigBit, tuple<IdString,IdString,int>> bit_drivers_db;
	dict<SigBit, pool<tuple<IdString,IdString,int>>> bit_users_db;

	SplitfanoutWorker(Module *module) : module(module), sigmap(module)
	{
		for (auto cell : module->cells()) {
			for (auto conn : cell->connections()) {
				if (!cell->output(conn.first)) continue;
				for (int i = 0; i < GetSize(conn.second); i++) {
					SigBit bit(sigmap(conn.second[i]));
					bit_drivers_db[bit] = tuple<IdString,IdString,int>(cell->name, conn.first, i);
				}
			}
		}

		for (auto cell : module->cells()) {
			for (auto conn : cell->connections()) {
				if (!cell->input(conn.first)) continue;
				for (int i = 0; i < GetSize(conn.second); i++) {
					SigBit bit(sigmap(conn.second[i]));
					if (!bit_drivers_db.count(bit)) continue;
					bit_users_db[bit].insert(tuple<IdString,IdString,int>(cell->name,
							conn.first, i-std::get<2>(bit_drivers_db[bit])));
				}
			}
		}

		for (auto wire : module->wires()) {
			if (!wire->port_output) continue;
			SigSpec sig(sigmap(wire));
			for (int i = 0; i < GetSize(sig); i++) {
				SigBit bit(sig[i]);
				if (!bit_drivers_db.count(bit)) continue;
				bit_users_db[bit].insert(tuple<IdString,IdString,int>(wire->name,
						IdString(), i-std::get<2>(bit_drivers_db[bit])));
			}
		}
	}

	int split(Cell *cell)
	{
		// Get output signal/port
		SigSpec outsig;
		IdString outport;
		int output_count = 0;
		for (auto conn : cell->connections())
			if (cell->output(conn.first)) {
				output_count++;
				outport = conn.first;
				outsig = conn.second;
			}
		if (output_count != 1) {
			log_debug("Skipping %s cell %s/%s with %d output ports.\n", log_id(cell->type), log_id(module), log_id(cell), output_count);
			return 0;
		}
		
		// Check if output signal is "bit-split", skip if so
		auto bit_users = bit_users_db[outsig[0]];
		for (int i = 0; i < GetSize(outsig); i++) {
			if (bit_users_db[outsig[i]] != bit_users) {
				log_debug("Skipping %s cell %s/%s with bit-split output.\n", log_id(cell->type), log_id(module), log_id(cell));
				return 0;
			}
		}

		// Skip if output signal has only one user
		if (GetSize(bit_users) <= 1)
			return 0;

		// Iterate over bit users and create a new cell for each one
		log_debug("Splitting %s cell %s/%s into %d copies based on fanout\n", log_id(cell->type), log_id(module), log_id(cell), GetSize(bit_users)-1);
		int foi = 0;
		cell->unsetPort(outport);
		int num_new_cells = GetSize(bit_users)-1;
		int bit_user_i = num_new_cells;
		for (auto bit_user : bit_users)
		{
			// Configure the driver cell
			IdString new_name;
			Cell *new_cell;
			if (bit_user_i-- != 0) { // create a new cell
				new_name = module->uniquify(cell->name.str());
				new_cell = module->addCell(new_name, cell);
				// Add new cell to the bit_users_db
				for (auto conn : new_cell->connections()) {
					if (!new_cell->input(conn.first)) continue;
					for (int i = 0; i < GetSize(conn.second); i++) {
						SigBit bit(sigmap(conn.second[i]));
						if (!bit_drivers_db.count(bit)) continue;
						bit_users_db[bit].insert(tuple<IdString,IdString,int>(new_cell->name,
								conn.first, i-std::get<2>(bit_drivers_db[bit])));
					}
				}
			} else { // if last cell, reuse the original cell
				new_name = cell->name;
				new_cell = cell;
			}

			// Connect the new cell to the user
			if (std::get<1>(bit_user) == IdString()) { // is wire
				Wire *old_wire = module->wire(std::get<0>(bit_user));
				Wire *new_wire = module->addWire(NEW_ID, old_wire);
				module->swap_names(old_wire, new_wire);
				old_wire->port_input = false;
				old_wire->port_output = false;
				SigSpec sig(new_wire, std::get<2>(bit_user), GetSize(outsig));
				new_cell->setPort(outport, sig);
			}
			else {
				Wire *new_wire = module->addWire(NEW_ID, GetSize(outsig));
				Cell *target_cell = module->cell(std::get<0>(bit_user));
				SigSpec sig = target_cell->getPort(std::get<1>(bit_user));
				sig.replace(std::get<2>(bit_user), new_wire);
				module->cell(std::get<0>(bit_user))->setPort(std::get<1>(bit_user), sig);
				new_cell->setPort(outport, new_wire);
			}

			// Log the new cell
			log_debug("  slice %d: %s => %s\n", foi++, log_id(new_name), log_signal(new_cell->getPort(outport)));
		}

		// Fix up ports
		module->fixup_ports();

		// Return the number of new cells created
		return num_new_cells;
	}
};

struct SplitfanoutPass : public Pass {
	SplitfanoutPass() : Pass("splitfanout", "split up cells with >1 fanout into copies") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    splitfanout [selection]\n");
		log("\n");
		log("This command copies selected cells with >1 fanout into cells with fanout 1. It\n");
		log("is effectively the opposite of the opt_merge pass.\n");
		log("\n");
		log("This command operates only on cells with 1 output and no 'bit split' on that\n");
		log("output.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing SPLITFANOUT pass (splitting up cells with >1 fanout into copies).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			// No options currently. When adding in the future make sure to update docstring with [options]
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules())
		{
			int count_split_pre = 0;
			int count_split_post = 0;

			while (1) {
				SplitfanoutWorker worker(module);
				bool did_something = false;
				for (auto cell : module->selected_cells()) {
					int n = worker.split(cell);
					did_something |= (n != 0);
					count_split_pre += (n != 0);
					count_split_post += n;
				}
				if (!did_something)
					break;
			}

			if (count_split_pre)
				log("Split %d cells in module '%s' into %d copies based on fanout.\n",
					count_split_pre, log_id(module), count_split_post);
		}

		Pass::call(design, "clean *");
	}
} SplitfanoutPass;

PRIVATE_NAMESPACE_END
