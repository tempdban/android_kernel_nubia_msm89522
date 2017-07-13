/* Copyright (c) 2015 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/device.h>

#include "pmic-voter.h"

#define NUM_MAX_CLIENTS	8

struct client_vote {
	int	state;
	int	value;
};

struct votable {
	struct client_vote	votes[NUM_MAX_CLIENTS];
	struct device		*dev;
	int			num_clients;
	int			type;
	int			effective_client_id;
	int			effective_result;
	struct mutex		vote_lock;
	int			(*callback)(struct device *dev,
						int effective_result,
						int effective_client,
						int last_result,
						int last_client);
};

static int vote_set_any(struct votable *votable)
{
	int i;

	for (i = 0; i < votable->num_clients; i++)
		if (votable->votes[i].state)
			return 1;
	return 0;
}

static int vote_min(struct votable *votable)
{
	int min_vote = INT_MAX;
	int client_index;
	int i;

	for (i = 0; i < votable->num_clients; i++) {
		if (votable->votes[i].state &&
				min_vote > votable->votes[i].value) {
			min_vote = votable->votes[i].value;
			client_index = i;
		}
	}

	return client_index;
}

static int vote_max(struct votable *votable)
{
	int max_vote = INT_MIN;
	int client_index;
	int i;

	for (i = 0; i < votable->num_clients; i++) {
		if (votable->votes[i].state &&
				max_vote < votable->votes[i].value) {
			max_vote = votable->votes[i].value;
			client_index = i;
		}
	}

	return client_index;
}

void lock_votable(struct votable *votable)
{
	mutex_lock(&votable->vote_lock);
}

void unlock_votable(struct votable *votable)
{
	mutex_unlock(&votable->vote_lock);
}

int get_client_vote(struct votable *votable, int client_id)
{
	int value;

	lock_votable(votable);
	value = get_client_vote_locked(votable, client_id);
	unlock_votable(votable);
	return value;
}

int get_client_vote_locked(struct votable *votable, int client_id)
{
	return votable->votes[client_id].value;
}

int get_effective_result(struct votable *votable)
{
	int value;

	lock_votable(votable);
	value = get_effective_result_locked(votable);
	unlock_votable(votable);
	return value;
}

int get_effective_result_locked(struct votable *votable)
{
	return votable->effective_result;
}

int get_effective_client_id(struct votable *votable)
{
	int id;

	lock_votable(votable);
	id = get_effective_client_id_locked(votable);
	unlock_votable(votable);
	return id;
}

int get_effective_client_id_locked(struct votable *votable)
{
	return votable->effective_client_id;
}

int vote(struct votable *votable, int client_id, int state, int val)
{
	int effective_id, effective_result;
	int rc = 0;

	lock_votable(votable);

	votable->votes[client_id].state = state;
	votable->votes[client_id].value = val;

	switch (votable->type) {
	case VOTE_MIN:
		effective_id = vote_min(votable);
		break;
	case VOTE_MAX:
		effective_id = vote_max(votable);
		break;
	case VOTE_SET_ANY:
		votable->votes[client_id].value = state;
		effective_result = vote_set_any(votable);
		if (effective_result != votable->effective_result) {
			votable->effective_client_id = client_id;
			votable->effective_result = effective_result;
			rc = votable->callback(votable->dev,
						effective_result, client_id,
						state, client_id);
		}
		goto out;
	default:
		rc = -EINVAL;
		goto vote_err;
	}

	effective_result = votable->votes[effective_id].value;

	if (effective_result != votable->effective_result) {
		votable->effective_client_id = effective_id;
		votable->effective_result = effective_result;
		rc = votable->callback(votable->dev, effective_result,
					effective_id, val, client_id);
	}
	goto out;

vote_err:
	pr_err("Invalid votable type specified for voter\n");
out:
	unlock_votable(votable);
	return rc;
}

struct votable *create_votable(struct device *dev, int votable_type,
					int num_clients,
					int (*callback)(struct device *dev,
							int effective_result,
							int effective_client,
							int last_result,
							int last_client)
					)
{
	struct votable *votable = devm_kzalloc(dev, sizeof(struct votable),
							GFP_KERNEL);

	if (!votable)
		return ERR_PTR(-ENOMEM);

	if (!callback) {
		dev_err(dev, "Invalid callback specified for voter\n");
		return ERR_PTR(-EINVAL);
	}

	if (votable_type >= NUM_VOTABLE_TYPES) {
		dev_err(dev, "Invalid votable_type specified for voter\n");
		return ERR_PTR(-EINVAL);
	}

	if (num_clients > NUM_MAX_CLIENTS) {
		dev_err(dev, "Invalid num_clients specified for voter\n");
		return ERR_PTR(-EINVAL);
	}

	votable->dev = dev;
	votable->num_clients = num_clients;
	votable->callback = callback;
	votable->type = votable_type;

	votable->effective_result = -1;
	mutex_init(&votable->vote_lock);

	return votable;
}
