/* $Id: ptest.c,v 1.17 2004/06/02 11:48:10 andrew Exp $ */

/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <portability.h>
#include <crm/crm.h>

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <crm/common/xml.h>
#include <crm/msg_xml.h>

#include <crm/cib.h>

#define OPTARGS	"V?i:o:D:C:S:HA:U:M:I:EWRFt:m:a:d:w:c:r:p:s:"

#include <getopt.h>
#include <glib.h>
#include <pengine.h>
#include <pe_utils.h>


int
main(int argc, char **argv)
{
	xmlNodePtr cib_object = NULL;
	int lpc = 0;
	int argerr = 0;
	int flag;
  
	cl_log_set_entity("ptest");
	cl_log_enable_stderr(TRUE);
	cl_log_set_facility(LOG_USER);

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			// Top-level Options
			{"daemon", 0, 0, 0},
      
			{0, 0, 0, 0}
		};
    
		flag = getopt_long(argc, argv, OPTARGS,
				   long_options, &option_index);
		if (flag == -1)
			break;
    
		switch(flag) {
			case 0:
				printf("option %s", long_options[option_index].name);
				if (optarg)
					printf(" with arg %s", optarg);
				printf("\n");
    
				break;
      
				/* a sample test for multiple instance
				   if (digit_optind != 0 && digit_optind != this_option_optind)
				   printf ("digits occur in two different argv-elements.\n");
				   digit_optind = this_option_optind;
				   printf ("option %c\n", c);
				*/
      
			case 'V':
				printf("option %d", flag);
				break;
			default:
				printf("?? getopt returned character code 0%o ??\n", flag);
				++argerr;
				break;
		}
	}
  
	if (optind < argc) {
		printf("non-option ARGV-elements: ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		printf("\n");
	}
  
	if (optind > argc) {
		++argerr;
	}
  
	if (argerr) {
		cl_log(LOG_ERR, "%d errors in option parsing", argerr);
	}
  
	cl_log(LOG_INFO, "=#=#=#=#= Getting XML =#=#=#=#=");
  
	cib_object = file2xml(stdin);
  
	cl_log(LOG_INFO, "=#=#=#=#= Stage 0 =#=#=#=#=");

	GSListPtr resources = NULL;
	GSListPtr nodes = NULL;
	GSListPtr node_constraints = NULL;
	GSListPtr actions = NULL;
	GSListPtr action_constraints = NULL;
	GSListPtr stonith_list = NULL;
	GSListPtr shutdown_list = NULL;

	GSListPtr colors = NULL;
	GSListPtr action_sets = NULL;

	xmlNodePtr graph = NULL;

	mtrace();
	pe_debug_on();
	
	stage0(cib_object,
	       &resources,
	       &nodes,  &node_constraints,
	       &actions,  &action_constraints,
	       &stonith_list, &shutdown_list);
	
	cl_log(LOG_INFO, "========= Nodes =========");
	slist_iter(node, node_t, nodes, lpc,
		   print_node(NULL, node, TRUE));

	cl_log(LOG_INFO, "========= Resources =========");
	slist_iter(resource, resource_t, resources, lpc,
		   print_resource(NULL, resource, TRUE));    

	cl_log(LOG_INFO, "========= Constraints =========");
	slist_iter(constraint, rsc_to_node_t, node_constraints, lpc,
		   print_rsc_to_node(NULL, constraint, FALSE));
    
	cl_log(LOG_INFO, "=#=#=#=#= Stage 1 =#=#=#=#=");
	stage1(node_constraints, nodes, resources);

	cl_log(LOG_INFO, "========= Nodes =========");
	slist_iter(node, node_t, nodes, lpc,
		   print_node(NULL, node, TRUE));

	cl_log(LOG_INFO, "========= Resources =========");
	slist_iter(resource, resource_t, resources, lpc,
		   print_resource(NULL, resource, TRUE));

	cl_log(LOG_INFO, "=#=#=#=#= Stage 2 =#=#=#=#=");
//	pe_debug_on();
	stage2(resources, nodes, &colors);
//	pe_debug_off();

	cl_log(LOG_INFO, "========= Nodes =========");
	slist_iter(node, node_t, nodes, lpc,
		   print_node(NULL, node, TRUE));

	cl_log(LOG_INFO, "========= Resources =========");
	slist_iter(resource, resource_t, resources, lpc,
		   print_resource(NULL, resource, TRUE));  
  
	cl_log(LOG_INFO, "========= Colors =========");
	slist_iter(color, color_t, colors, lpc,
		   print_color(NULL, color, FALSE));
  
	cl_log(LOG_INFO, "=#=#=#=#= Stage 3 =#=#=#=#=");
	stage3(colors);
	cl_log(LOG_INFO, "========= Colors =========");
	slist_iter(color, color_t, colors, lpc,
		   print_color(NULL, color, FALSE));

	cl_log(LOG_INFO, "=#=#=#=#= Stage 4 =#=#=#=#=");
	stage4(colors);
	cl_log(LOG_INFO, "========= Colors =========");
	slist_iter(color, color_t, colors, lpc,
		   print_color(NULL, color, FALSE));

	cl_log(LOG_INFO, "=#=#=#=#= Summary =#=#=#=#=");
	summary(resources);
	cl_log(LOG_INFO, "========= Action List =========");
	slist_iter(action, action_t, actions, lpc,
		   print_action(NULL, action, FALSE));
	
	cl_log(LOG_INFO, "=#=#=#=#= Stage 5 =#=#=#=#=");
	stage5(resources);

	cl_log(LOG_INFO, "=#=#=#=#= Stage 6 =#=#=#=#=");
	stage6(&actions, &action_constraints,
	       stonith_list, shutdown_list);

	cl_log(LOG_INFO, "========= Action List =========");
	slist_iter(action, action_t, actions, lpc,
		   print_action(NULL, action, TRUE));
	
	cl_log(LOG_INFO, "=#=#=#=#= Stage 7 =#=#=#=#=");
	stage7(resources, actions, action_constraints, &action_sets);

	cl_log(LOG_INFO, "=#=#=#=#= Summary =#=#=#=#=");
	summary(resources);

	cl_log(LOG_INFO, "========= All Actions =========");
	slist_iter(action, action_t, actions, lpc,
		   print_action("\t", action, TRUE);
		);

	cl_log(LOG_INFO, "========= Action Sets =========");

	cl_log(LOG_INFO, "\t========= Set %d (Un-runnable) =========", -1);
	slist_iter(action, action_t, actions, lpc,
		   if(action->optional == FALSE && action->runnable == FALSE) {
			   print_action("\t", action, TRUE);
		   }
		);

	int lpc2;
	slist_iter(action_set, GSList, action_sets, lpc,
		   cl_log(LOG_INFO, "\t========= Set %d =========", lpc);
		   slist_iter(action, action_t, action_set, lpc2,
			      print_action("\t", action, TRUE)));

	
	cl_log(LOG_INFO, "========= Stonith List =========");
	slist_iter(node, node_t, stonith_list, lpc,
		   print_node(NULL, node, FALSE));
  
	cl_log(LOG_INFO, "========= Shutdown List =========");
	slist_iter(node, node_t, shutdown_list, lpc,
		   print_node(NULL, node, FALSE));

	cl_log(LOG_INFO, "=#=#=#=#= Stage 8 =#=#=#=#=");
	stage8(action_sets, &graph);

//	GSListPtr action_sets = NULL;


	pdebug("deleting node cons");
	while(node_constraints) {
		pe_free_rsc_to_node((rsc_to_node_t*)node_constraints->data);
		node_constraints = node_constraints->next;
	}
	g_slist_free(node_constraints);

	pdebug("deleting order cons");
	pe_free_shallow(action_constraints);

	pdebug("deleting action sets");

	slist_iter(action_set, GSList, action_sets, lpc,
		   pe_free_shallow_adv(action_set, FALSE);
		);
	pe_free_shallow_adv(action_sets, FALSE);
	
	pdebug("deleting actions");
	pe_free_actions(actions);

	pdebug("deleting resources");
	pe_free_resources(resources); 
	
	pdebug("deleting colors");
	pe_free_colors(colors);

	crm_free(no_color->details);
	crm_free(no_color);
	
	pdebug("deleting nodes");
	pe_free_nodes(nodes);
	
	g_slist_free(shutdown_list);
	g_slist_free(stonith_list);

	pe_debug_off();
	muntrace();

	char *msg_buffer = dump_xml_node(graph, FALSE);
	fprintf(stdout, "%s\n", msg_buffer);
	fflush(stdout);
	crm_free(msg_buffer);

	
	return 0;
}
