////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Arc Welder: Anti-Stutter Library
//
// Compresses many G0/G1 commands into G2/G3(arc) commands where possible, ensuring the tool paths stay within the specified resolution.
// This reduces file size and the number of gcodes per second.
//
// Uses the 'Gcode Processor Library' for gcode parsing, position processing, logging, and other various functionality.
//
// Copyright(C) 2020 - Brad Hochgesang
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// This program is free software : you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU Affero General Public License for more details.
//
//
// You can contact the author at the following email address: 
// FormerLurker@pm.me
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if _MSC_VER > 1200
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include "arc_welder.h"
#include <vector>
#include <sstream>
#include "utilities.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>

arc_welder::arc_welder(
	std::string source_path, 
	std::string target_path, 
	logger * log, 
	double resolution_mm, 
	double path_tolerance_percent,
	double max_radius,
	int min_arc_segments,
	double mm_per_arc_segment,
	bool g90_g91_influences_extruder, 
	bool allow_3d_arcs,
	bool allow_dynamic_precision,
	unsigned char default_xyz_precision,
	unsigned char default_e_precision,
	int buffer_size, 
	progress_callback callback) : current_arc_(
			DEFAULT_MIN_SEGMENTS, 
			buffer_size - 5, 
			resolution_mm, 
			path_tolerance_percent, 
			max_radius,
			min_arc_segments,
			mm_per_arc_segment,
			allow_3d_arcs,
			default_xyz_precision,
			default_e_precision
		), 
		segment_statistics_(
			segment_statistic_lengths, 
			segment_statistic_lengths_count, 
			log
		)
{
	p_logger_ = log;
	debug_logging_enabled_ = false;
	info_logging_enabled_ = false;
	error_logging_enabled_ = false;
	verbose_logging_enabled_ = false;

	logger_type_ = 0;
	resolution_mm_ = resolution_mm;
	progress_callback_ = callback;
	verbose_output_ = false;
	source_path_ = source_path;
	target_path_ = target_path;
	gcode_position_args_ = get_args_(g90_g91_influences_extruder, buffer_size);
	allow_3d_arcs_ = allow_3d_arcs;
	allow_dynamic_precision_ = allow_dynamic_precision;
	notification_period_seconds = 1;
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	file_size_ = 0;
	last_gcode_line_written_ = 0;
	points_compressed_ = 0;
	arcs_created_ = 0;
	waiting_for_arc_ = false;
	previous_feedrate_ = -1;
	previous_is_extruder_relative_ = false;
	gcode_position_args_.set_num_extruders(8);
	for (int index = 0; index < 8; index++)
	{
		gcode_position_args_.retraction_lengths[0] = .0001;
		gcode_position_args_.z_lift_heights[0] = 0.001;
		gcode_position_args_.x_firmware_offsets[0] = 0.0;
		gcode_position_args_.y_firmware_offsets[0] = 0.0;
	}

	// We don't care about the printer settings, except for g91 influences extruder.
	
	p_source_position_ = new gcode_position(gcode_position_args_); 
}

gcode_position_args arc_welder::get_args_(bool g90_g91_influences_extruder, int buffer_size)
{
	gcode_position_args args;
	// Configure gcode_position_args
	args.g90_influences_extruder = g90_g91_influences_extruder;
	args.position_buffer_size = buffer_size;
	args.autodetect_position = true;
	args.home_x = 0;
	args.home_x_none = true;
	args.home_y = 0;
	args.home_y_none = true;
	args.home_z = 0;
	args.home_z_none = true;
	args.shared_extruder = true;
	args.zero_based_extruder = true;


	args.default_extruder = 0;
	args.xyz_axis_default_mode = "absolute";
	args.e_axis_default_mode = "absolute";
	args.units_default = "millimeters";
	args.location_detection_commands = std::vector<std::string>();
	args.is_bound_ = false;
	args.is_circular_bed = false;
	args.x_min = -9999;
	args.x_max = 9999;
	args.y_min = -9999;
	args.y_max = 9999;
	args.z_min = -9999;
	args.z_max = 9999;
	return args;
}

arc_welder::~arc_welder()
{
	delete p_source_position_;
}

void arc_welder::set_logger_type(int logger_type)
{
	logger_type_ = logger_type;
}

void arc_welder::reset()
{
	p_logger_->log(logger_type_, DEBUG, "Resetting all tracking variables.");
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	last_gcode_line_written_ = 0;
	file_size_ = 0;
	points_compressed_ = 0;
	arcs_created_ = 0;
	waiting_for_arc_ = false;
}

long arc_welder::get_file_size(const std::string& file_path)
{
	// Todo:  Fix this function.  This is a pretty weak implementation :(
	std::ifstream file(file_path.c_str(), std::ios::in | std::ios::binary);
	const long l = (long)file.tellg();
	file.seekg(0, std::ios::end);
	const long m = (long)file.tellg();
	file.close();
	return (m - l);
}

double arc_welder::get_next_update_time() const
{
	return clock() + (notification_period_seconds * CLOCKS_PER_SEC);
}

double arc_welder::get_time_elapsed(double start_clock, double end_clock)
{
	return static_cast<double>(end_clock - start_clock) / CLOCKS_PER_SEC;
}

arc_welder_results arc_welder::process()
{
arc_welder_results results;
	p_logger_->log(logger_type_, DEBUG, "Configuring logging settings.");
	verbose_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, VERBOSE);
	debug_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, DEBUG);
	info_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, INFO);
	error_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, ERROR);

	std::stringstream stream;
	stream << std::fixed << std::setprecision(2);
	stream << "arc_welder::process - Parameters received: source_file_path: '" <<
		source_path_ << "', target_file_path:'" << target_path_ << "', resolution_mm:" <<
		resolution_mm_ << "mm (+-" << current_arc_.get_resolution_mm() << "mm), path_tolerance_percent: " << current_arc_.get_path_tolerance_percent()  
		<< ", max_radius_mm:" << current_arc_.get_max_radius()
		<< ", min_arc_segments:" << std::setprecision(0) <<current_arc_.get_min_arc_segments() 
		<< ", mm_per_arc_segment:" << std::setprecision(0) << current_arc_.get_mm_per_arc_segment()
		<< ", g90_91_influences_extruder: " << (p_source_position_->get_g90_91_influences_extruder() ? "True" : "False")
		<< ", allow_3d_arcs: " << (allow_3d_arcs_ ? "True" : "False")
		<< ", allow_dynamic_precision: " << (allow_dynamic_precision_ ? "True" : "False")
		<< ", default_xyz_precision: " << std::setprecision(0) << static_cast<double>(current_arc_.get_xyz_precision())
		<< ", default_e_precision: " << std::setprecision(0) << static_cast<double>(current_arc_.get_e_precision());
	p_logger_->log(logger_type_, INFO, stream.str());


	// reset tracking variables
	reset();
	// local variable to hold the progress update return.  If it's false, we will exit.
	bool continue_processing = true;
	
	p_logger_->log(logger_type_, DEBUG, "Configuring progress updates.");
	int read_lines_before_clock_check = 1000;
	double next_update_time = get_next_update_time();
	const clock_t start_clock = clock();
	p_logger_->log(logger_type_, DEBUG, "Getting source file size.");
	file_size_ = get_file_size(source_path_);
	stream.clear();
	stream.str("");
	stream << "Source file size: " << file_size_;
	p_logger_->log(logger_type_, DEBUG, stream.str());
	// Create the source file read stream and target write stream
	std::ifstream gcodeFile;
	p_logger_->log(logger_type_, DEBUG, "Opening the source file for reading.");
	gcodeFile.open(source_path_.c_str(), std::ifstream::in);
	if (!gcodeFile.is_open())
	{
		results.success = false;
		results.message = "Unable to open the source file.";
		p_logger_->log_exception(logger_type_, results.message);
		return results;
	}
	p_logger_->log(logger_type_, DEBUG, "Source file opened successfully.");

	p_logger_->log(logger_type_, DEBUG, "Opening the target file for writing.");

	output_file_.open(target_path_.c_str(), std::ifstream::out);
	if (!output_file_.is_open())
	{
		results.success = false;
		results.message = "Unable to open the target file.";
		p_logger_->log_exception(logger_type_, results.message);
		gcodeFile.close();
		return results;
	}
	
	p_logger_->log(logger_type_, DEBUG, "Target file opened successfully.");
	std::string line;
	int lines_with_no_commands = 0;
	//gcodeFile.sync_with_stdio(false);
	//output_file_.sync_with_stdio(false);
	
	add_arcwelder_comment_to_target();
	
	parsed_command cmd;
	// Communicate every second
	p_logger_->log(logger_type_, DEBUG, "Sending initial progress update.");
	continue_processing = on_progress_(get_progress_(static_cast<long>(gcodeFile.tellg()), static_cast<double>(start_clock)));
	p_logger_->log(logger_type_, DEBUG, "Processing source file.");
	while (std::getline(gcodeFile, line) && continue_processing)
	{
		lines_processed_++;

		cmd.clear();
		if (verbose_logging_enabled_)
		{
			stream.clear();
			stream.str("");
			stream << "Parsing: " << line;
			p_logger_->log(logger_type_, VERBOSE, stream.str());
		}
		parser_.try_parse_gcode(line.c_str(), cmd, true);
		bool has_gcode = false;
		if (cmd.gcode.length() > 0)
		{
			has_gcode = true;
			gcodes_processed_++;
		}
		else
		{
			lines_with_no_commands++;
		}

		// Always process the command through the printer, even if no command is found
		// This is important so that comments can be analyzed
		//std::cout << "stabilization::process_file - updating position...";
		process_gcode(cmd, false, false);

		// Only continue to process if we've found a command and either a progress_callback_ is supplied, or debug loggin is enabled.
		if (has_gcode)
		{
			if ((lines_processed_ % read_lines_before_clock_check) == 0 && next_update_time < clock())
			{
				if (verbose_logging_enabled_)
				{
					p_logger_->log(logger_type_, VERBOSE, "Sending progress update.");
				}
				continue_processing = on_progress_(get_progress_(static_cast<long>(gcodeFile.tellg()), static_cast<double>(start_clock)));
				next_update_time = get_next_update_time();
			}
		}
	}

	if (current_arc_.is_shape() && waiting_for_arc_)
	{
		p_logger_->log(logger_type_, DEBUG, "The target file opened successfully.");
		process_gcode(cmd, true, false);
	}
	p_logger_->log(logger_type_, DEBUG, "Writing all unwritten gcodes to the target file.");
	write_unwritten_gcodes_to_file();
	p_logger_->log(logger_type_, DEBUG, "Fetching the final progress struct.");

	arc_welder_progress final_progress = get_progress_(static_cast<long>(file_size_), static_cast<double>(start_clock));
	if (debug_logging_enabled_)
	{
		p_logger_->log(logger_type_, DEBUG, "Sending final progress update message.");
	}
	on_progress_(arc_welder_progress(final_progress));
	
	p_logger_->log(logger_type_, DEBUG, "Processing complete, closing source and target file.");
	output_file_.close();
	gcodeFile.close();
	const clock_t end_clock = clock();
	
	results.success = continue_processing;
	results.cancelled = !continue_processing;
	results.progress = final_progress;
	p_logger_->log(logger_type_, DEBUG, "Returning processing results.");

	return results;
}

bool arc_welder::on_progress_(const arc_welder_progress& progress)
{
	if (progress_callback_ != NULL)
	{
		return progress_callback_(progress, p_logger_, logger_type_);
	}
	else if (info_logging_enabled_)
	{
		p_logger_->log(logger_type_, INFO, progress.str());
	}

	return true;
}

arc_welder_progress arc_welder::get_progress_(long source_file_position, double start_clock)
{
	arc_welder_progress progress;
	progress.gcodes_processed = gcodes_processed_;
	progress.lines_processed = lines_processed_;
	progress.points_compressed = points_compressed_;
	progress.arcs_created = arcs_created_;
	progress.source_file_position = source_file_position;
	progress.target_file_size = static_cast<long>(output_file_.tellp());
	progress.source_file_size = file_size_;
	long bytesRemaining = file_size_ - static_cast<long>(source_file_position);
	progress.percent_complete = static_cast<double>(source_file_position) / static_cast<double>(file_size_) * 100.0;
	progress.seconds_elapsed = get_time_elapsed(start_clock, clock());
	double bytesPerSecond = static_cast<double>(source_file_position) / progress.seconds_elapsed;
	progress.seconds_remaining = bytesRemaining / bytesPerSecond;

	if (source_file_position > 0) {
		progress.compression_ratio = (static_cast<float>(source_file_position) / static_cast<float>(progress.target_file_size));
		progress.compression_percent = (1.0 - (static_cast<float>(progress.target_file_size) / static_cast<float>(source_file_position))) * 100.0f;
	}
	progress.num_firmware_compensations = current_arc_.get_num_firmware_compensations();
	progress.segment_statistics = segment_statistics_;
	return progress;
	
}

int arc_welder::process_gcode(parsed_command cmd, bool is_end, bool is_reprocess)
{
	/* use to catch gcode for debugging since I can't set conditions equal to strings
	if (cmd.gcode == "G1 X118.762 Y104.054 E0.0163")
	{
		std::cout << "Found it!";
	}
	*/
	// Update the position for the source gcode file
	p_source_position_->update(cmd, lines_processed_, gcodes_processed_, -1);
	position* p_cur_pos = p_source_position_->get_current_position_ptr();
	position* p_pre_pos = p_source_position_->get_previous_position_ptr();
	extruder extruder_current = p_cur_pos->get_current_extruder();
	extruder previous_extruder = p_pre_pos->get_current_extruder();
	//std::cout << lines_processed_ << " - " << cmd.gcode << ", CurrentEAbsolute: " << cur_extruder.e <<", ExtrusionLength: " << cur_extruder.extrusion_length << ", Retraction Length: " << cur_extruder.retraction_length << ", IsExtruding: " << cur_extruder.is_extruding << ", IsRetracting: " << cur_extruder.is_retracting << ".\n";

	int lines_written = 0;
	// see if this point is an extrusion
	
	bool arc_added = false;
	bool clear_shapes = false;
	double movement_length_mm = 0;
	bool has_e_changed = extruder_current.is_extruding || extruder_current.is_retracting;
	// Update the source file statistics
	if (p_cur_pos->has_xy_position_changed && (has_e_changed))
	{
		if (allow_3d_arcs_) {
			movement_length_mm = utilities::get_cartesian_distance(p_pre_pos->x, p_pre_pos->y, p_pre_pos->z, p_cur_pos->x, p_cur_pos->y, p_cur_pos->z);
		}
		else {
			movement_length_mm = utilities::get_cartesian_distance(p_pre_pos->x, p_pre_pos->y, p_cur_pos->x, p_cur_pos->y);
		}
		
		if (movement_length_mm > 0)
		{
			if (!is_reprocess)
				segment_statistics_.update(movement_length_mm, true);
		}
	}

	// We need to make sure the printer is using absolute xyz, is extruding, and the extruder axis mode is the same as that of the previous position
	// TODO: Handle relative XYZ axis.  This is possible, but maybe not so important.
	bool is_g1_g2 = cmd.command == "G0" || cmd.command == "G1";
	if (allow_dynamic_precision_ && is_g1_g2)
	{
		for (std::vector<parsed_command_parameter>::iterator it = cmd.parameters.begin(); it != cmd.parameters.end(); ++it)
		{
			switch ((*it).name[0])
			{
				case 'X':
				case 'Y':
				case 'Z':
					current_arc_.update_xyz_precision((*it).double_precision);
					break;
				case 'E':
					current_arc_.update_e_precision((*it).double_precision);
					break;
			}
		}
	}
	
	bool z_axis_ok = allow_3d_arcs_ ||
		utilities::is_equal(p_cur_pos->z, p_pre_pos->z);

	if (
		!is_end && cmd.is_known_command && !cmd.is_empty && (
			is_g1_g2 && z_axis_ok &&
			utilities::is_equal(p_cur_pos->x_offset, p_pre_pos->x_offset) &&
			utilities::is_equal(p_cur_pos->y_offset, p_pre_pos->y_offset) &&
			utilities::is_equal(p_cur_pos->z_offset, p_pre_pos->z_offset) &&
			utilities::is_equal(p_cur_pos->x_firmware_offset, p_pre_pos->x_firmware_offset) &&
			utilities::is_equal(p_cur_pos->y_firmware_offset, p_pre_pos->y_firmware_offset) &&
			utilities::is_equal(p_cur_pos->z_firmware_offset, p_pre_pos->z_firmware_offset) &&
			!p_cur_pos->is_relative &&
			(
				!waiting_for_arc_ ||
				extruder_current.is_extruding ||
				//(previous_extruder.is_extruding && extruder_current.is_extruding) || // Test to see if 
				// we can get more arcs.
				(previous_extruder.is_retracting && extruder_current.is_retracting)
			) &&
			p_cur_pos->is_extruder_relative == p_pre_pos->is_extruder_relative &&
			(!waiting_for_arc_ || p_pre_pos->f == p_cur_pos->f) && // might need to skip the waiting for arc check...
			(!waiting_for_arc_ || p_pre_pos->feature_type_tag == p_cur_pos->feature_type_tag)
			)
	) {

		printer_point p(p_cur_pos->get_gcode_x(), p_cur_pos->get_gcode_y(), p_cur_pos->get_gcode_z(), extruder_current.e_relative, movement_length_mm);
		if (!waiting_for_arc_)
		{
			previous_is_extruder_relative_ = p_pre_pos->is_extruder_relative;
			if (debug_logging_enabled_)
			{
				p_logger_->log(logger_type_, DEBUG, "Starting new arc from Gcode:" + cmd.gcode);
			}
			write_unwritten_gcodes_to_file();
			// add the previous point as the starting point for the current arc
			printer_point previous_p(p_pre_pos->get_gcode_x(), p_pre_pos->get_gcode_y(), p_pre_pos->get_gcode_z(), previous_extruder.e_relative, 0);
			// Don't add any extrusion, or you will over extrude!
			//std::cout << "Trying to add first point (" << p.x << "," << p.y << "," << p.z << ")...";
			
			current_arc_.try_add_point(previous_p);
		}
		
		double e_relative = extruder_current.e_relative;
		int num_points = current_arc_.get_num_segments();
		arc_added = current_arc_.try_add_point(p);
		if (arc_added)
		{
			if (!waiting_for_arc_)
			{
				waiting_for_arc_ = true;
				previous_feedrate_ = p_pre_pos->f;
			}
			else
			{
				if (debug_logging_enabled_)
				{
					if (num_points+1 == current_arc_.get_num_segments())
					{
						p_logger_->log(logger_type_, DEBUG, "Adding point to arc from Gcode:" + cmd.gcode);
					}
					
				}
			}
		}
	}
	else if (debug_logging_enabled_ ){
		if (is_end)
		{
			p_logger_->log(logger_type_, DEBUG, "Procesing final shape, if one exists.");
		}
		else if (!cmd.is_empty)
		{
			if (!cmd.is_known_command)
			{
				p_logger_->log(logger_type_, DEBUG, "Command '" + cmd.command + "' is Unknown.  Gcode:" + cmd.gcode);
			}
			else if (cmd.command != "G0" && cmd.command != "G1")
			{
				p_logger_->log(logger_type_, DEBUG, "Command '"+ cmd.command + "' is not G0/G1, skipping.  Gcode:" + cmd.gcode);
			}
			else if (!allow_3d_arcs_ && !utilities::is_equal(p_cur_pos->z, p_pre_pos->z))
			{
				p_logger_->log(logger_type_, DEBUG, "Z axis position changed, cannot convert:" + cmd.gcode);
			}
			else if (p_cur_pos->is_relative)
			{
				p_logger_->log(logger_type_, DEBUG, "XYZ Axis is in relative mode, cannot convert:" + cmd.gcode);
			}
			else if (
				waiting_for_arc_ && !( 
					(previous_extruder.is_extruding && extruder_current.is_extruding) ||
					(previous_extruder.is_retracting && extruder_current.is_retracting)
				)
			)
			{
				std::string message = "Extruding or retracting state changed, cannot add point to current arc: " + cmd.gcode;
				if (verbose_logging_enabled_)
				{
					
					message.append(
						" - Verbose Info\n\tCurrent Position Info - Absolute E:" + utilities::to_string(extruder_current.e) +
						", Offset E:" + utilities::to_string(extruder_current.get_offset_e()) +
						", Mode:" + (p_cur_pos->is_extruder_relative_null ? "NULL" : p_cur_pos->is_extruder_relative ? "relative" : "absolute") +
						", Retraction: " + utilities::to_string(extruder_current.retraction_length) +
						", Extrusion: " + utilities::to_string(extruder_current.extrusion_length) +
						", Retracting: " + (extruder_current.is_retracting ? "True" : "False") +
						", Extruding: " + (extruder_current.is_extruding ? "True" : "False")
					);
					message.append(
						"\n\tPrevious Position Info - Absolute E:" + utilities::to_string(previous_extruder.e) +
						", Offset E:" + utilities::to_string(previous_extruder.get_offset_e()) +
						", Mode:" + (p_pre_pos->is_extruder_relative_null ? "NULL" : p_pre_pos->is_extruder_relative ? "relative" : "absolute") +
						", Retraction: " + utilities::to_string(previous_extruder.retraction_length) +
						", Extrusion: " + utilities::to_string(previous_extruder.extrusion_length) +
						", Retracting: " + (previous_extruder.is_retracting ? "True" : "False") +
						", Extruding: " + (previous_extruder.is_extruding ? "True" : "False")
					);
					p_logger_->log(logger_type_, VERBOSE, message);
				}
				else
				{
					p_logger_->log(logger_type_, DEBUG, message);
				}
				
			}
			else if (p_cur_pos->is_extruder_relative != p_pre_pos->is_extruder_relative)
			{
				p_logger_->log(logger_type_, DEBUG, "Extruder axis mode changed, cannot add point to current arc: " + cmd.gcode);
			}
			else if (waiting_for_arc_ && p_pre_pos->f != p_cur_pos->f)
			{
				p_logger_->log(logger_type_, DEBUG, "Feedrate changed, cannot add point to current arc: " + cmd.gcode);
			}
			else if (waiting_for_arc_ && p_pre_pos->feature_type_tag != p_cur_pos->feature_type_tag)
			{
				p_logger_->log(logger_type_, DEBUG, "Feature type changed, cannot add point to current arc: " + cmd.gcode);
			}
			else
			{
				// Todo:  Add all the relevant values
				p_logger_->log(logger_type_, DEBUG, "There was an unknown issue preventing the current point from being added to the arc: " + cmd.gcode);
			}
		}
	}
	
	if (!arc_added && !(cmd.is_empty && cmd.comment.length() == 0))
	{
		if (current_arc_.get_num_segments() < current_arc_.get_min_segments()) {
			if (debug_logging_enabled_ && !cmd.is_empty)
			{
				if (current_arc_.get_num_segments() != 0)
				{
					p_logger_->log(logger_type_, DEBUG, "Not enough segments, resetting. Gcode:" + cmd.gcode);
				}
				
			}
			waiting_for_arc_ = false;
			current_arc_.clear();
		}
		else if (waiting_for_arc_)
		{

			if (current_arc_.is_shape())
			{
				// update our statistics
				points_compressed_ += current_arc_.get_num_segments()-1;
				arcs_created_++; // increment the number of generated arcs
				write_arc_gcodes(p_pre_pos->is_extruder_relative, p_pre_pos->f);
				p_cur_pos = NULL;
				p_pre_pos = NULL;
				

				// Reprocess this line
				if (!is_end)
				{
					return process_gcode(cmd, false, true);
				}
				else
				{
					if (debug_logging_enabled_)
					{
						p_logger_->log(logger_type_, DEBUG, "Final arc created, exiting.");
					}
					return 0;
				}
					
			}
			else
			{
				if (debug_logging_enabled_)
				{
					p_logger_->log(logger_type_, DEBUG, "The current arc is not a valid arc, resetting.");
				}
				current_arc_.clear();
				waiting_for_arc_ = false;
			}
		}
		else if (debug_logging_enabled_)
		{
			p_logger_->log(logger_type_, DEBUG, "Could not add point to arc from gcode:" + cmd.gcode);
		}

	}
	
	if (waiting_for_arc_ || !arc_added)
	{
		position* cur_pos = p_source_position_->get_current_position_ptr();
		unwritten_commands_.push_back(unwritten_command(cur_pos, movement_length_mm));
		
	}
	if (!waiting_for_arc_)
	{
		write_unwritten_gcodes_to_file();
	}
	return lines_written;
}

void arc_welder::write_arc_gcodes(bool is_extruder_relative, double current_feedrate)
{

	std::string comment = get_comment_for_arc();
	// remove the same number of unwritten gcodes as there are arc segments, minus 1 for the start point
	// Which isn't a movement
	// note, skip the first point, it is the starting point
	for (int index = 0; index < current_arc_.get_num_segments() - 1; index++)
	{
		unwritten_commands_.pop_back();
	}
	
	// Undo the current command, since it isn't included in the arc
	p_source_position_->undo_update();
	
	// Set the current feedrate if it is different, else set to 0 to indicate that no feedrate should be included
	if (previous_feedrate_ > 0 && previous_feedrate_ == current_feedrate) {
		current_feedrate = 0;
	}

	// Craete the arc gcode
	std::string gcode;
	if (previous_is_extruder_relative_) {
		gcode = get_arc_gcode_relative(current_feedrate, comment);
	}

	else {
		gcode = get_arc_gcode_absolute(p_source_position_->get_current_position_ptr()->get_current_extruder().get_offset_e(), current_feedrate, comment);
	}


	if (debug_logging_enabled_)
	{
		char buffer[20];
		std::string message = "Arc created with ";
		sprintf(buffer, "%d", current_arc_.get_num_segments());
		message += buffer;
		message += " segments: ";
		message += gcode;
		p_logger_->log(logger_type_, DEBUG, message);
	}

	// Write everything that hasn't yet been written	
	write_unwritten_gcodes_to_file();

	// Update the current extrusion statistics for the current arc gcode
	segment_statistics_.update(current_arc_.get_shape_length() , false);
	// now write the current arc to the file 
	write_gcode_to_file(gcode);

	// Now clear the arc and flag the processor as not waiting for an arc
	waiting_for_arc_ = false;
	current_arc_.clear();
}

std::string arc_welder::get_comment_for_arc()
{
	// build a comment string from the commands making up the arc
				// We need to start with the first command entered.
	int comment_index = unwritten_commands_.count() - (current_arc_.get_num_segments() - 1);
	std::string comment;
	for (; comment_index < unwritten_commands_.count(); comment_index++)
	{
		std::string old_comment = unwritten_commands_[comment_index].comment;
		if (old_comment != comment && old_comment.length() > 0)
		{
			if (comment.length() > 0)
			{
				comment += " - ";
			}
			comment += old_comment;
		}
	}
	return comment;
}

std::string arc_welder::create_g92_e(double absolute_e)
{
	std::stringstream stream;
	stream << std::fixed << std::setprecision(5);
	stream << "G92 E" << absolute_e;
	return stream.str();
}

int arc_welder::write_gcode_to_file(std::string gcode)
{
	output_file_ << gcode << "\n";
	return 1;
}

int arc_welder::write_unwritten_gcodes_to_file()
{
	int size = unwritten_commands_.count();
	std::string lines_to_write;
	
	for (int index = 0; index < size; index++)
	{
		// The the current unwritten position and remove it from the list
		unwritten_command p = unwritten_commands_.pop_front();
		if (p.extrusion_length > 0)
		{
			segment_statistics_.update(p.extrusion_length, false);
		}
		lines_to_write.append(p.to_string()).append("\n");
	}
	
	output_file_ << lines_to_write;
	return size;
}

std::string arc_welder::get_arc_gcode_relative(double f, const std::string comment)
{
	// Write gcode to file
	std::string gcode;

	gcode = current_arc_.get_shape_gcode_relative(f);
	
	if (comment.length() > 0)
	{
		gcode += ";" + comment;
	}
	return gcode;
	
}

std::string arc_welder::get_arc_gcode_absolute(double e, double f, const std::string comment)
{
	// Write gcode to file
	std::string gcode;

	gcode = current_arc_.get_shape_gcode_absolute(e, f);

	if (comment.length() > 0)
	{
		gcode += ";" + comment;
	}
	return gcode;

}

void arc_welder::add_arcwelder_comment_to_target()
{
	p_logger_->log(logger_type_, DEBUG, "Adding ArcWelder comment to the target file.");
	std::stringstream stream;
	stream << std::fixed;
	stream <<	"; Postprocessed by [ArcWelder](https://github.com/FormerLurker/ArcWelderLib)\n";
	stream << "; Copyright(C) 2020 - Brad Hochgesang\n";
	stream << "; resolution=" << std::setprecision(2) << resolution_mm_ << "mm\n";
	stream << "; path_tolerance=" << std::setprecision(0) << (current_arc_.get_path_tolerance_percent() * 100.0) << "%\n";
	stream << "; max_radius=" << std::setprecision(2) << (current_arc_.get_max_radius()) << "mm\n";
	if (gcode_position_args_.g90_influences_extruder)
	{
		stream << "; g90_influences_extruder=True\n";
	}
	if (current_arc_.get_mm_per_arc_segment() > 0 && current_arc_.get_min_arc_segments() > 0)
	{							 																																									
		stream << "; firmware_compensation=True\n";
		stream << "; mm_per_arc_segment="<< std::setprecision(2) << current_arc_.get_mm_per_arc_segment() << "mm\n";
		stream << "; min_arc_segments=" << std::setprecision(0) << current_arc_.get_min_arc_segments() << "\n";
	}
	if (allow_3d_arcs_)
	{
		stream << "; allow_3d_arcs=True\n";

	}
	if (allow_dynamic_precision_)
	{
		stream << "; allow_dynamic_precision=True\n";
	}
	stream << "; default_xyz_precision=" << std::setprecision(0) << static_cast<int>(current_arc_.get_xyz_precision()) << "\n";
	stream << "; default_e_precision=" << std::setprecision(0) << static_cast<int>(current_arc_.get_e_precision()) << "\n\n";

	
	output_file_ << stream.str();
}


